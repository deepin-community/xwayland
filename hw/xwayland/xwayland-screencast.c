/*
 * Copyright (C) 2026
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation.
 */

#include <xwayland-config.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include <dbus/dbus.h>
#include <pipewire/pipewire.h>
#include <spa/buffer/buffer.h>
#include <spa/param/buffers.h>
#include <spa/param/format-utils.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/raw-utils.h>
#include <spa/pod/vararg.h>
#include <spa/utils/result.h>

#include <X11/X.h>
#include <X11/Xatom.h>

#include "client.h"
#include "fb.h"
#include "damage.h"
#include "dixstruct.h"
#include "gcstruct.h"
#include "os.h"
#include "pixmapstr.h"
#include "property.h"
#include "windowstr.h"
#include "window.h"
#include "xserver_poll.h"

#include "xwayland-screencast-protocol.h"
#include "xwayland-screencast.h"

#define PORTAL_BUS_NAME "org.freedesktop.portal.Desktop"
#define PORTAL_OBJECT_PATH "/org/freedesktop/portal/desktop"
#define PORTAL_SCREENCAST_IFACE "org.freedesktop.portal.ScreenCast"
#define PORTAL_REQUEST_IFACE "org.freedesktop.portal.Request"
#define PORTAL_PROPERTIES_IFACE "org.freedesktop.DBus.Properties"

#define PORTAL_SCREENCAST_SOURCE_MONITOR 1
#define PORTAL_SCREENCAST_CURSOR_HIDDEN 1
#define PORTAL_SCREENCAST_CURSOR_EMBEDDED 2

#define XWL_SCREENCAST_START_TIMEOUT_MS 30000
#define XWL_SCREENCAST_HELPER_IO_TIMEOUT_MS 5000
#define XWL_SCREENCAST_HELPER_PAYLOAD_TIMEOUT_MS 30000
#define XWL_SCREENCAST_DBUS_CALL_TIMEOUT_MS 5000
#define XWL_SCREENCAST_DBUS_CLOSE_TIMEOUT_MS 1000
#define XWL_SCREENCAST_BPP 4
#define XWL_SCREENCAST_BUFFER_TYPES (1u << SPA_DATA_MemFd)
#define XWL_SCREENCAST_MAX_FRAME_SIZE (1ULL << 30)

enum xwl_screencast_frame_status {
    XWL_SCREENCAST_FRAME_OK,
    XWL_SCREENCAST_FRAME_NO_FRAME,
    XWL_SCREENCAST_FRAME_ERROR,
};

struct xwl_screencast_stream {
    struct xwl_screencast *screencast;

    uint32_t node_id;
    uint64_t pipewire_serial;
    int x;
    int y;
    int width;
    int height;

    struct pw_stream *pw_stream;
    struct spa_hook stream_listener;
    struct spa_video_info_raw raw;

    uint8_t *frame;
    size_t frame_size;
    int frame_width;
    int frame_height;
    int frame_stride;
    enum spa_video_format frame_format;
    Bool frame_valid;
    Bool logged_unsupported_buffer;
    Bool logged_short_buffer;
};

struct xwl_screencast_pending {
    struct xwl_screencast_pending *next;
    struct xwl_screencast *screencast;
    ClientPtr client;
    uint64_t client_serial;
    int request_sequence;
    pthread_t thread;
    Bool thread_started;
    Bool completed;
    Bool cancelled;
    Bool signaled;
    int fd;
    uid_t uid;
    char *app_id;
    char *app_name;
    enum xwl_screencast_frame_status status;
    struct xwl_screencast_helper_response response;
    uint8_t *frame;
};

struct xwl_screencast_client {
    uint64_t serial;
    pid_t pid;
    uid_t uid;
    Bool uid_valid;
    char *bus_address;
    struct xwl_screencast_pending *pending;
};

struct xwl_screencast {
    struct xwl_screen *xwl_screen;

    pthread_mutex_t mutex;
    pthread_cond_t cond;
    pthread_t thread;

    Bool started;
    Bool thread_finished;
    Bool stop;
    Bool failed;
    Bool have_frame;
    Bool logged_helper_unavailable;
    Bool logged_no_frame;
    Bool logged_no_overlap;
    const char *stage;

    int notify_fds[2];

    DBusConnection *connection;
    char *session_handle;
    char *response_match;
    char *bus_address;
    pid_t bus_pid;
    uid_t bus_uid;
    Bool bus_uid_valid;

    struct pw_thread_loop *pw_loop;
    struct pw_context *pw_context;
    struct pw_core *pw_core;

    struct xwl_screencast_stream *streams;
    uint32_t n_streams;

    uint64_t helper_frame_client_serial;
    int helper_frame_request_sequence;
    Bool helper_frame_key_valid;
    enum xwl_screencast_frame_status helper_frame_status;

    struct xwl_screencast_pending *pending;
};

static DevPrivateKeyRec xwl_screencast_client_private_key;
static Bool xwl_screencast_client_private_registered;
static Bool xwl_screencast_client_callback_registered;
static uint64_t xwl_screencast_next_client_serial;

struct xwl_screencast_app_lookup {
    ClientPtr client;
    pid_t pid;
    char *wm_instance;
    char *wm_class;
};

static int64_t xwl_screencast_monotonic_msec(void);
static void *
xwl_screencast_thread(void *data);
static void
xwl_screencast_pending_destroy(struct xwl_screencast_pending *pending);

static void _X_ATTRIBUTE_PRINTF(1, 2)
xwl_screencast_log(const char *format, ...)
{
    char message[1024];
    va_list args;

    va_start(args, format);
    vsnprintf(message, sizeof message, format, args);
    va_end(args);

    ErrorF("%s\n", message);
    openlog("Xwayland", LOG_PID, LOG_USER);
    syslog(LOG_INFO, "%s", message);
}

static void
xwl_screencast_error(const char *message)
{
    xwl_screencast_log("Xwayland screencast portal: %s", message);
}

static const char *
xwl_screencast_frame_status_name(enum xwl_screencast_frame_status status)
{
    switch (status) {
    case XWL_SCREENCAST_FRAME_OK:
        return "ok";
    case XWL_SCREENCAST_FRAME_NO_FRAME:
        return "no-frame";
    case XWL_SCREENCAST_FRAME_ERROR:
    default:
        return "error";
    }
}

static Bool
xwl_screencast_stopped(struct xwl_screencast *screencast)
{
    Bool stopped;

    pthread_mutex_lock(&screencast->mutex);
    stopped = screencast->stop;
    pthread_mutex_unlock(&screencast->mutex);

    return stopped;
}

static void
xwl_screencast_set_failed(struct xwl_screencast *screencast)
{
    pthread_mutex_lock(&screencast->mutex);
    screencast->failed = TRUE;
    pthread_cond_broadcast(&screencast->cond);
    pthread_mutex_unlock(&screencast->mutex);
}

static void
xwl_screencast_set_stage(struct xwl_screencast *screencast,
                         const char *stage)
{
    pthread_mutex_lock(&screencast->mutex);
    screencast->stage = stage;
    pthread_cond_broadcast(&screencast->cond);
    pthread_mutex_unlock(&screencast->mutex);
}

static void
xwl_screencast_signal_frame(struct xwl_screencast *screencast)
{
    char byte = 1;

    screencast->have_frame = TRUE;
    pthread_cond_broadcast(&screencast->cond);

    if (screencast->notify_fds[1] >= 0) {
        ssize_t written = write(screencast->notify_fds[1], &byte, sizeof byte);

        (void) written;
    }
}

static void
xwl_screencast_wake_main(struct xwl_screencast *screencast)
{
    char byte = 1;

    if (screencast->notify_fds[1] >= 0) {
        ssize_t written = write(screencast->notify_fds[1], &byte, sizeof byte);

        (void) written;
    }
}

static void
xwl_screencast_notify(int fd, int ready, void *data)
{
    struct xwl_screencast *screencast = data;
    ScreenPtr screen = screencast->xwl_screen->screen;
    WindowPtr root = screen->root;
    char buf[64];
    BoxRec box;
    RegionRec region;

    while (read(fd, buf, sizeof buf) > 0) {
    }

    for (;;) {
        struct xwl_screencast_pending *pending;
        struct xwl_screencast_pending *orphan = NULL;
        ClientPtr client = NULL;

        pthread_mutex_lock(&screencast->mutex);
        for (pending = screencast->pending; pending; pending = pending->next) {
            if (!pending->completed)
                continue;

            if (!pending->client) {
                orphan = pending;
                break;
            }

            if (!pending->signaled) {
                pending->signaled = TRUE;
                client = pending->client;
                break;
            }
        }
        pthread_mutex_unlock(&screencast->mutex);

        if (orphan) {
            xwl_screencast_pending_destroy(orphan);
            continue;
        }

        if (!client)
            break;

        if (!ClientSignal(client)) {
            pthread_mutex_lock(&screencast->mutex);
            for (pending = screencast->pending; pending; pending = pending->next) {
                if (pending->client == client && pending->completed) {
                    struct xwl_screencast_client *sc_client =
                        dixLookupPrivate(&client->devPrivates,
                                         &xwl_screencast_client_private_key);

                    if (sc_client && sc_client->pending == pending)
                        sc_client->pending = NULL;
                    pending->client = NULL;
                    break;
                }
            }
            pthread_mutex_unlock(&screencast->mutex);
        }
    }

    if (!root)
        return;

    box.x1 = 0;
    box.y1 = 0;
    box.x2 = screen->width;
    box.y2 = screen->height;
    RegionInit(&region, &box, 1);
    DamageDamageRegion(&root->drawable, &region);
    RegionUninit(&region);
}

static void
xwl_screencast_make_token(char *buf, size_t size, const char *prefix)
{
    static uint32_t serial;

    snprintf(buf, size, "%s_%u_%u", prefix, (unsigned) getpid(), ++serial);
}

static Bool
set_fd_flags(int fd)
{
    int flags;

    flags = fcntl(fd, F_GETFD);
    if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0)
        return FALSE;

    flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        return FALSE;

    return TRUE;
}

static char *
xwl_screencast_strdup_range(const char *str, size_t len)
{
    char *copy = malloc(len + 1);

    if (!copy)
        return NULL;

    memcpy(copy, str, len);
    copy[len] = '\0';
    return copy;
}

static Bool
xwl_screencast_valid_app_id_char(char c)
{
    return (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') ||
           c == '_';
}

static char *
xwl_screencast_app_name_from_string(const char *value)
{
    char name[XWL_SCREENCAST_HELPER_APP_NAME_SIZE];
    size_t i, j = 0;

    if (!value || value[0] == '\0')
        value = "Xwayland Application";

    for (i = 0; value[i] != '\0' && j + 1 < sizeof name; i++) {
        unsigned char c = value[i];

        if (c < 0x20 || c == 0x7f)
            c = ' ';
        name[j++] = c;
    }

    while (j > 0 && name[j - 1] == ' ')
        j--;
    name[j] = '\0';

    if (name[0] == '\0')
        snprintf(name, sizeof name, "%s", "Xwayland Application");

    return strdup(name);
}

static char *
xwl_screencast_app_id_from_string(const char *value)
{
    char app_id[XWL_SCREENCAST_HELPER_APP_ID_SIZE];
    size_t prefix_len;
    size_t i, j;

    snprintf(app_id, sizeof app_id, "%s", "xwayland.");
    prefix_len = strlen(app_id);
    j = prefix_len;

    if (!value || value[0] == '\0')
        value = "unknown";

    for (i = 0; value[i] != '\0' && j + 1 < sizeof app_id; i++) {
        char c = value[i];

        if (xwl_screencast_valid_app_id_char(c))
            app_id[j++] = c;
        else if (j > prefix_len && app_id[j - 1] != '_')
            app_id[j++] = '_';
    }

    while (j > prefix_len && app_id[j - 1] == '_')
        j--;
    if (j == prefix_len) {
        snprintf(app_id + prefix_len, sizeof app_id - prefix_len, "%s",
                 "unknown");
    } else {
        app_id[j] = '\0';
    }

    return strdup(app_id);
}

static char * _X_ATTRIBUTE_PRINTF(1, 2)
xwl_screencast_strdup_printf(const char *format, ...)
{
    va_list args;
    va_list args_copy;
    char *str;
    int len;

    va_start(args, format);
    va_copy(args_copy, args);
    len = vsnprintf(NULL, 0, format, args);
    va_end(args);

    if (len < 0) {
        va_end(args_copy);
        return NULL;
    }

    str = malloc(len + 1);
    if (!str) {
        va_end(args_copy);
        return NULL;
    }

    vsnprintf(str, len + 1, format, args_copy);
    va_end(args_copy);
    return str;
}

static char *
xwl_screencast_read_client_environ(pid_t pid, size_t *len_out)
{
    char path[64];
    char *buf = NULL;
    size_t len = 0;
    size_t size = 4096;
    int fd;

    *len_out = 0;

    if (pid <= 0)
        return NULL;

    snprintf(path, sizeof path, "/proc/%ld/environ", (long) pid);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return NULL;

    buf = malloc(size + 1);
    if (!buf) {
        close(fd);
        return NULL;
    }

    for (;;) {
        ssize_t nread;

        if (len == size) {
            char *new_buf;

            if (size >= 1024 * 1024) {
                free(buf);
                close(fd);
                return NULL;
            }

            size *= 2;
            new_buf = realloc(buf, size + 1);
            if (!new_buf) {
                free(buf);
                close(fd);
                return NULL;
            }
            buf = new_buf;
        }

        nread = read(fd, buf + len, size - len);
        if (nread < 0) {
            if (errno == EINTR)
                continue;

            free(buf);
            close(fd);
            return NULL;
        }
        if (nread == 0)
            break;

        len += nread;
    }

    close(fd);
    buf[len] = '\0';
    *len_out = len;
    return buf;
}

static char *
xwl_screencast_environ_lookup(const char *env, size_t env_len,
                              const char *key)
{
    size_t key_len = strlen(key);
    size_t pos = 0;

    while (pos < env_len) {
        const char *entry = env + pos;
        size_t entry_len = strnlen(entry, env_len - pos);

        if (entry_len > key_len &&
            entry[key_len] == '=' &&
            memcmp(entry, key, key_len) == 0) {
            const char *value = entry + key_len + 1;

            if (value[0] == '\0')
                return NULL;

            return xwl_screencast_strdup_range(value,
                                               entry_len - key_len - 1);
        }

        pos += entry_len + 1;
    }

    return NULL;
}

static Bool
xwl_screencast_get_client_creds(ClientPtr client, pid_t *pid_out,
                                uid_t *uid_out, Bool *uid_valid_out)
{
    LocalClientCredRec *lcc = NULL;
    Bool have_creds = FALSE;

    *pid_out = -1;
    *uid_out = 0;
    *uid_valid_out = FALSE;

    if (!client)
        return FALSE;

    if (GetLocalClientCreds(client, &lcc) == 0) {
        if (lcc->fieldsSet & LCC_PID_SET) {
            *pid_out = lcc->pid;
            have_creds = TRUE;
        }
        if (lcc->fieldsSet & LCC_UID_SET) {
            *uid_out = lcc->euid;
            *uid_valid_out = TRUE;
            have_creds = TRUE;
        }
        FreeLocalClientCreds(lcc);
    }

    return have_creds;
}

static Bool
xwl_screencast_window_wm_class(WindowPtr window,
                               char **instance_out,
                               char **class_out)
{
    PropertyPtr prop = NULL;
    const char *data;
    const char *instance;
    const char *class_name;
    size_t len;
    size_t instance_len;
    size_t class_len = 0;
    int ret;

    *instance_out = NULL;
    *class_out = NULL;

    if (!window)
        return FALSE;

    ret = dixLookupProperty(&prop, window, XA_WM_CLASS,
                            serverClient, DixReadAccess);
    if (ret != Success || !prop || prop->type != XA_STRING ||
        prop->format != 8 || prop->size == 0 || !prop->data)
        return FALSE;

    data = prop->data;
    len = prop->size;
    instance = data;
    instance_len = strnlen(instance, len);

    class_name = NULL;
    if (instance_len < len) {
        size_t remaining = len - instance_len - 1;

        class_name = instance + instance_len + 1;
        class_len = strnlen(class_name, remaining);
        if (class_len == 0)
            class_name = NULL;
    }

    if (instance_len > 0)
        *instance_out = xwl_screencast_strdup_range(instance, instance_len);

    if (class_name)
        *class_out = xwl_screencast_strdup_range(class_name, class_len);

    if (!*instance_out && !*class_out)
        return FALSE;

    return TRUE;
}

static int
xwl_screencast_app_lookup_visit(WindowPtr window, void *data)
{
    struct xwl_screencast_app_lookup *lookup = data;
    ClientPtr owner;

    if (window == window->drawable.pScreen->root)
        return WT_WALKCHILDREN;

    owner = wClient(window);
    if (owner == lookup->client ||
        (lookup->pid > 0 && owner && owner != serverClient &&
         GetClientPid(owner) == lookup->pid)) {
        if (xwl_screencast_window_wm_class(window,
                                           &lookup->wm_instance,
                                           &lookup->wm_class))
            return WT_STOPWALKING;
    }

    return WT_WALKCHILDREN;
}

static Bool
xwl_screencast_client_wm_class(struct xwl_screen *xwl_screen,
                               ClientPtr client,
                               pid_t pid,
                               char **instance_out,
                               char **class_out)
{
    struct xwl_screencast_app_lookup lookup = {
        .client = client,
        .pid = -1,
    };

    *instance_out = NULL;
    *class_out = NULL;

    if (!xwl_screen || !xwl_screen->screen || !xwl_screen->screen->root ||
        !client)
        return FALSE;

    /* Prefer a window owned by the requesting X connection. */
    WalkTree(xwl_screen->screen, xwl_screencast_app_lookup_visit, &lookup);
    if (!lookup.wm_instance && !lookup.wm_class && pid > 0) {
        /* Some toolkits use multiple X connections from the same process. */
        lookup.pid = pid;
        WalkTree(xwl_screen->screen, xwl_screencast_app_lookup_visit, &lookup);
    }
    *instance_out = lookup.wm_instance;
    *class_out = lookup.wm_class;
    return *instance_out || *class_out;
}

static char *
xwl_screencast_client_comm(pid_t pid)
{
    char path[64];
    char buf[256];
    ssize_t len;
    int fd;

    if (pid <= 0)
        return NULL;

    snprintf(path, sizeof path, "/proc/%ld/comm", (long) pid);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return NULL;

    len = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (len <= 0)
        return NULL;

    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\0'))
        len--;
    if (len <= 0)
        return NULL;

    buf[len] = '\0';
    return strdup(buf);
}

static const char *
xwl_screencast_identity_basename(const char *identity)
{
    const char *base;

    if (!identity || identity[0] == '\0')
        return identity;

    base = strrchr(identity, '/');
    if (base && base[1] != '\0')
        return base + 1;

    return identity;
}

static Bool
xwl_screencast_cached_client_creds(ClientPtr client, pid_t *pid_out,
                                  uid_t *uid_out, Bool *uid_valid_out)
{
    struct xwl_screencast_client *sc_client;
    pid_t pid = -1;
    uid_t uid = 0;
    Bool uid_valid = FALSE;

    *pid_out = -1;
    *uid_out = 0;
    *uid_valid_out = FALSE;

    if (!client)
        return FALSE;

    sc_client = dixLookupPrivate(&client->devPrivates,
                                 &xwl_screencast_client_private_key);
    if (sc_client) {
        if ((sc_client->pid <= 0 || !sc_client->uid_valid) &&
            xwl_screencast_get_client_creds(client, &pid, &uid,
                                            &uid_valid)) {
            if (sc_client->pid <= 0 && pid > 0)
                sc_client->pid = pid;
            if (!sc_client->uid_valid && uid_valid) {
                sc_client->uid = uid;
                sc_client->uid_valid = TRUE;
            }
        }
        if (sc_client->pid <= 0)
            sc_client->pid = GetClientPid(client);

        *pid_out = sc_client->pid;
        *uid_out = sc_client->uid;
        *uid_valid_out = sc_client->uid_valid;
        return sc_client->pid > 0 || sc_client->uid_valid;
    }

    xwl_screencast_get_client_creds(client, pid_out, uid_out,
                                    uid_valid_out);
    if (*pid_out <= 0)
        *pid_out = GetClientPid(client);

    return *pid_out > 0 || *uid_valid_out;
}

static char *
xwl_screencast_client_app_id(struct xwl_screen *xwl_screen,
                             ClientPtr client,
                             pid_t pid,
                             char **app_name_out)
{
    char *identity = NULL;
    char *wm_instance = NULL;
    char *wm_class = NULL;
    const char *app_name = NULL;
    char *app_id;

    xwl_screencast_client_wm_class(xwl_screen, client, pid,
                                   &wm_instance, &wm_class);
    if (wm_instance)
        identity = strdup(wm_instance);
    if (!identity && wm_class)
        identity = strdup(wm_class);
    if (!identity && client) {
        const char *cmdname = GetClientCmdName(client);

        if (cmdname)
            identity = strdup(xwl_screencast_identity_basename(cmdname));
    }
    if (!identity)
        identity = xwl_screencast_client_comm(pid);
    if (!identity)
        identity = strdup("unknown");
    if (!identity)
        return NULL;

    app_id = xwl_screencast_app_id_from_string(identity);
    if (app_name_out) {
        app_name = wm_class ? wm_class : xwl_screencast_identity_basename(identity);
        *app_name_out = xwl_screencast_app_name_from_string(app_name);
    }
    free(wm_instance);
    free(wm_class);
    free(identity);

    return app_id;
}

static char *
xwl_screencast_bus_address_from_environ(pid_t pid)
{
    char *env;
    size_t env_len;
    char *address;
    char *runtime_dir;

    env = xwl_screencast_read_client_environ(pid, &env_len);
    if (!env)
        return NULL;

    address = xwl_screencast_environ_lookup(env, env_len,
                                            "DBUS_SESSION_BUS_ADDRESS");
    if (address) {
        free(env);
        return address;
    }

    runtime_dir = xwl_screencast_environ_lookup(env, env_len,
                                                "XDG_RUNTIME_DIR");
    if (runtime_dir) {
        address = xwl_screencast_strdup_printf("unix:path=%s/bus",
                                               runtime_dir);
        free(runtime_dir);
        free(env);
        return address;
    }

    free(env);
    return NULL;
}

static void
xwl_screencast_client_free(struct xwl_screencast_client *client_info)
{
    if (!client_info)
        return;

    free(client_info->bus_address);
    free(client_info);
}

static void
xwl_screencast_client_state_callback(CallbackListPtr *pcbl, void *data,
                                     void *call_data)
{
    NewClientInfoRec *clientinfo = call_data;
    ClientPtr client = clientinfo->client;
    struct xwl_screencast_client *sc_client;
    struct xwl_screencast_pending *completed_pending = NULL;

    switch (client->clientState) {
    case ClientStateRunning:
        sc_client = calloc(1, sizeof *sc_client);
        if (!sc_client)
            return;

        xwl_screencast_next_client_serial++;
        if (xwl_screencast_next_client_serial == 0)
            xwl_screencast_next_client_serial++;
        sc_client->serial = xwl_screencast_next_client_serial;

        xwl_screencast_get_client_creds(client,
                                        &sc_client->pid,
                                        &sc_client->uid,
                                        &sc_client->uid_valid);
        if (sc_client->pid <= 0)
            sc_client->pid = GetClientPid(client);
        sc_client->bus_address =
            xwl_screencast_bus_address_from_environ(sc_client->pid);

        dixSetPrivate(&client->devPrivates,
                      &xwl_screencast_client_private_key,
                      sc_client);
        break;
    case ClientStateGone:
        sc_client = dixLookupPrivate(&client->devPrivates,
                                     &xwl_screencast_client_private_key);
        if (sc_client && sc_client->pending) {
            struct xwl_screencast_pending *pending = sc_client->pending;
            struct xwl_screencast *screencast = pending->screencast;

            pthread_mutex_lock(&screencast->mutex);
            if (pending->client == client) {
                pending->client = NULL;
                pending->cancelled = TRUE;
                if (pending->fd >= 0)
                    shutdown(pending->fd, SHUT_RDWR);
                if (pending->completed)
                    completed_pending = pending;
            }
            sc_client->pending = NULL;
            pthread_mutex_unlock(&screencast->mutex);
        }
        xwl_screencast_client_free(sc_client);
        dixSetPrivate(&client->devPrivates,
                      &xwl_screencast_client_private_key,
                      NULL);
        if (completed_pending)
            xwl_screencast_pending_destroy(completed_pending);
        break;
    case ClientStateInitial:
    case ClientStateRetained:
        break;
    }
}

static char *
xwl_screencast_client_bus_address(ClientPtr client,
                                  pid_t *pid_out,
                                  uid_t *uid_out,
                                  Bool *uid_valid_out)
{
    struct xwl_screencast_client *sc_client;
    pid_t pid = -1;
    uid_t uid = 0;
    Bool uid_valid = FALSE;
    char *address = NULL;

    *pid_out = -1;
    *uid_out = 0;
    *uid_valid_out = FALSE;

    if (!client)
        return NULL;

    sc_client = dixLookupPrivate(&client->devPrivates,
                                 &xwl_screencast_client_private_key);
    if (sc_client) {
        pid = sc_client->pid;
        uid = sc_client->uid;
        uid_valid = sc_client->uid_valid;
        if (sc_client->bus_address)
            address = strdup(sc_client->bus_address);
    }

    if (!sc_client &&
        !xwl_screencast_get_client_creds(client, &pid, &uid, &uid_valid)) {
        xwl_screencast_log("Xwayland screencast portal: failed to get local "
                           "client credentials for session bus selection");
    } else {
        *pid_out = pid;
        *uid_out = uid;
        *uid_valid_out = uid_valid;
    }

    if (address)
        return address;

    address = xwl_screencast_bus_address_from_environ(pid);
    if (address)
        return address;

    if (uid_valid) {
        address = xwl_screencast_strdup_printf("unix:path=/run/user/%lu/bus",
                                               (unsigned long) uid);
        return address;
    }

    return NULL;
}

static void
xwl_screencast_set_client_bus(struct xwl_screencast *screencast,
                              ClientPtr client)
{
    pid_t pid;
    uid_t uid;
    Bool uid_valid;
    char *address;

    pthread_mutex_lock(&screencast->mutex);
    if (screencast->bus_address && screencast->started &&
        !screencast->thread_finished && !screencast->failed) {
        pthread_mutex_unlock(&screencast->mutex);
        return;
    }
    pthread_mutex_unlock(&screencast->mutex);

    address = xwl_screencast_client_bus_address(client, &pid, &uid, &uid_valid);
    if (!address)
        return;

    pthread_mutex_lock(&screencast->mutex);
    if (!screencast->bus_address ||
        strcmp(screencast->bus_address, address) != 0) {
        free(screencast->bus_address);
        screencast->bus_address = address;
        screencast->bus_pid = pid;
        screencast->bus_uid = uid;
        screencast->bus_uid_valid = uid_valid;
        screencast->logged_no_frame = FALSE;
        screencast->logged_helper_unavailable = FALSE;
        address = NULL;
    }
    pthread_mutex_unlock(&screencast->mutex);

    free(address);
}

static Bool
xwl_screencast_wait_fd(int fd, short events, int64_t deadline_msec)
{
    while (TRUE) {
        struct pollfd poll_fd = {
            .fd = fd,
            .events = events,
        };
        int64_t now;
        int timeout;
        int ret;

        if (deadline_msec < 0) {
            timeout = -1;
        } else {
            now = xwl_screencast_monotonic_msec();
            if (now >= deadline_msec) {
                errno = ETIMEDOUT;
                return FALSE;
            }

            if (deadline_msec - now > INT_MAX)
                timeout = INT_MAX;
            else
                timeout = deadline_msec - now;
        }
        ret = xserver_poll(&poll_fd, 1, timeout);
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            return FALSE;
        }
        if (ret == 0) {
            errno = ETIMEDOUT;
            return FALSE;
        }
        if (poll_fd.revents & events)
            return TRUE;
        if (poll_fd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            errno = ECONNRESET;
            return FALSE;
        }
    }
}

static Bool
xwl_screencast_read_full_timed(int fd, void *data, size_t size,
                               int64_t deadline_msec)
{
    uint8_t *ptr = data;

    while (size > 0) {
        ssize_t len;

        if (!xwl_screencast_wait_fd(fd, POLLIN, deadline_msec))
            return FALSE;

        len = read(fd, ptr, size);
        if (len < 0) {
            if (errno == EINTR)
                continue;
            return FALSE;
        }
        if (len == 0) {
            errno = ECONNRESET;
            return FALSE;
        }

        ptr += len;
        size -= len;
    }

    return TRUE;
}

static Bool
xwl_screencast_write_full_timed(int fd, const void *data, size_t size,
                                int64_t deadline_msec)
{
    const uint8_t *ptr = data;

    while (size > 0) {
        ssize_t len;

        if (!xwl_screencast_wait_fd(fd, POLLOUT, deadline_msec))
            return FALSE;

        len = write(fd, ptr, size);
        if (len < 0) {
            if (errno == EINTR)
                continue;
            return FALSE;
        }
        if (len == 0) {
            errno = ECONNRESET;
            return FALSE;
        }

        ptr += len;
        size -= len;
    }

    return TRUE;
}

static Bool
xwl_screencast_pread_full(int fd, void *data, size_t size, off_t offset)
{
    uint8_t *ptr = data;

    while (size > 0) {
        ssize_t len = pread(fd, ptr, size, offset);

        if (len < 0) {
            if (errno == EINTR)
                continue;
            return FALSE;
        }
        if (len == 0)
            return FALSE;

        ptr += len;
        size -= len;
        offset += len;
    }

    return TRUE;
}

static int64_t
xwl_screencast_monotonic_msec(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static uint32_t
xwl_screencast_next_request_id(void)
{
    static uint32_t serial;

    return ++serial;
}

static void
xwl_screencast_set_socket_timeout(int fd, int timeout_ms)
{
    struct timeval timeout;

    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
}

static int
xwl_screencast_connect_helper_abstract(uid_t uid)
{
    struct sockaddr_un addr;
    char name[sizeof(addr.sun_path) - 1];
    size_t name_len;
    socklen_t addr_len;
    int fd;

    snprintf(name, sizeof name, "%s%lu",
             XWL_SCREENCAST_HELPER_SOCKET_PREFIX, (unsigned long) uid);
    name_len = strlen(name);
    if (name_len + 1 > sizeof(addr.sun_path))
        return -1;

    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;

    xwl_screencast_set_socket_timeout(fd,
                                      XWL_SCREENCAST_HELPER_IO_TIMEOUT_MS);

    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    addr.sun_path[0] = '\0';
    memcpy(addr.sun_path + 1, name, name_len);
    addr_len = offsetof(struct sockaddr_un, sun_path) + 1 + name_len;

    if (connect(fd, (struct sockaddr *) &addr, addr_len) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static int
xwl_screencast_connect_helper_path(uid_t uid)
{
    struct sockaddr_un addr;
    char path[sizeof(addr.sun_path)];
    int fd;

    snprintf(path, sizeof path, "%s%lu%s",
             XWL_SCREENCAST_HELPER_PATH_PREFIX,
             (unsigned long) uid,
             XWL_SCREENCAST_HELPER_PATH_SUFFIX);

    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;

    xwl_screencast_set_socket_timeout(fd,
                                      XWL_SCREENCAST_HELPER_IO_TIMEOUT_MS);

    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path);

    if (connect(fd, (struct sockaddr *) &addr, sizeof addr) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static int
xwl_screencast_connect_helper(uid_t uid)
{
    int fd;

    fd = xwl_screencast_connect_helper_abstract(uid);
    if (fd >= 0)
        return fd;

    return xwl_screencast_connect_helper_path(uid);
}

static Bool
xwl_screencast_store_helper_frame(struct xwl_screencast *screencast,
                                  struct xwl_screencast_helper_response *resp,
                                  uint8_t *frame)
{
    struct xwl_screencast_stream *stream;
    struct xwl_screencast_stream *streams;
    ScreenPtr screen = screencast->xwl_screen->screen;
    int logical_width = screen ? screen->width : 0;
    int logical_height = screen ? screen->height : 0;

    pthread_mutex_lock(&screencast->mutex);

    if (screencast->n_streams == 0) {
        streams = reallocarray(screencast->streams, 1,
                               sizeof *screencast->streams);
        if (!streams) {
            pthread_mutex_unlock(&screencast->mutex);
            return FALSE;
        }

        screencast->streams = streams;
        screencast->n_streams = 1;
        memset(&screencast->streams[0], 0, sizeof screencast->streams[0]);
        screencast->streams[0].screencast = screencast;
    }

    stream = &screencast->streams[0];
    free(stream->frame);
    stream->frame = frame;
    stream->frame_size = resp->frame_size;
    stream->frame_width = resp->frame_width;
    stream->frame_height = resp->frame_height;
    stream->frame_stride = resp->frame_stride;
    stream->frame_format = resp->format;
    stream->frame_valid = TRUE;
    /*
     * Screenshot portal images use physical pixels, while root-window image
     * requests use Xwayland's logical coordinate space.  Treat the helper's
     * complete screenshot as the complete X root and let the copy path scale
     * between those spaces.
     */
    stream->x = 0;
    stream->y = 0;
    stream->width = logical_width;
    stream->height = logical_height;
    if (stream->width <= 0)
        stream->width = stream->frame_width;
    if (stream->height <= 0)
        stream->height = stream->frame_height;

    if (stream->frame_width != stream->width ||
        stream->frame_height != stream->height) {
        xwl_screencast_log("Xwayland screenshot portal: mapping physical "
                           "helper frame %dx%d to logical X root %dx%d",
                           stream->frame_width, stream->frame_height,
                           stream->width, stream->height);
    }

    xwl_screencast_signal_frame(screencast);
    pthread_mutex_unlock(&screencast->mutex);

    return TRUE;
}

static Bool
xwl_screencast_pending_set_fd(struct xwl_screencast_pending *pending, int fd)
{
    struct xwl_screencast *screencast = pending->screencast;
    Bool accepted;

    pthread_mutex_lock(&screencast->mutex);
    accepted = !pending->cancelled && !screencast->stop;
    if (accepted)
        pending->fd = fd;
    pthread_mutex_unlock(&screencast->mutex);

    return accepted;
}

static void
xwl_screencast_pending_close_fd(struct xwl_screencast_pending *pending, int fd)
{
    struct xwl_screencast *screencast = pending->screencast;

    pthread_mutex_lock(&screencast->mutex);
    if (pending->fd == fd)
        pending->fd = -1;
    pthread_mutex_unlock(&screencast->mutex);
    close(fd);
}

static enum xwl_screencast_frame_status
xwl_screencast_get_helper_frame(
    struct xwl_screencast_pending *pending,
    uid_t uid,
    const char *app_id,
    const char *app_name,
    struct xwl_screencast_helper_response *response_out,
    uint8_t **frame_out)
{
    struct xwl_screencast_helper_request req = {
        .magic = XWL_SCREENCAST_HELPER_MAGIC,
        .version = XWL_SCREENCAST_HELPER_VERSION,
        .opcode = XWL_SCREENCAST_HELPER_GET_FRAME,
    };
    struct xwl_screencast_helper_response resp;
    uint8_t *frame = NULL;
    int fd = -1;
    uint32_t request_id = xwl_screencast_next_request_id();
    int64_t start_ms = xwl_screencast_monotonic_msec();
    int64_t deadline_ms;
    uint64_t minimum_stride;
    uint64_t expected_size;
    enum xwl_screencast_frame_status status = XWL_SCREENCAST_FRAME_ERROR;

    memset(response_out, 0, sizeof *response_out);
    *frame_out = NULL;

    if (!app_id || app_id[0] == '\0')
        app_id = "xwayland.unknown";
    if (!app_name || app_name[0] == '\0')
        app_name = app_id;

    req.reserved = request_id;
    snprintf(req.app_id, sizeof req.app_id, "%s", app_id);
    snprintf(req.app_name, sizeof req.app_name, "%s", app_name);

    xwl_screencast_log("Xwayland screenshot portal: helper request id=%u "
                       "start uid=%ld app_id=%s app_name=%s "
                       "decision-timeout=none",
                       request_id, (long) uid, app_id, app_name);

    fd = xwl_screencast_connect_helper(uid);
    if (fd < 0) {
        xwl_screencast_log("Xwayland screenshot portal: helper request id=%u "
                           "failed to connect uid=%ld elapsed=%" PRId64 "ms",
                           request_id, (long) uid,
                           xwl_screencast_monotonic_msec() - start_ms);
        goto out;
    }
    if (!xwl_screencast_pending_set_fd(pending, fd))
        goto out;

    deadline_ms = start_ms + XWL_SCREENCAST_HELPER_IO_TIMEOUT_MS;

    if (!xwl_screencast_write_full_timed(fd, &req, sizeof req, deadline_ms)) {
        xwl_screencast_log("Xwayland screenshot portal: helper request id=%u "
                           "failed to send frame request timeout=%dms "
                           "error=%s elapsed=%" PRId64 "ms",
                           request_id, XWL_SCREENCAST_HELPER_IO_TIMEOUT_MS,
                           strerror(errno),
                           xwl_screencast_monotonic_msec() - start_ms);
        goto out;
    }

    /* The response header is produced only after the portal request has a
     * user decision.  Do not turn a slow decision into a synthetic denial;
     * shutdown() from cancellation still interrupts this wait. */
    if (!xwl_screencast_read_full_timed(fd, &resp, sizeof resp, -1)) {
        xwl_screencast_log("Xwayland screenshot portal: helper request id=%u "
                           "ended while waiting for a decision: %s "
                           "elapsed=%" PRId64 "ms",
                           request_id, strerror(errno),
                           xwl_screencast_monotonic_msec() - start_ms);
        goto out;
    }

    if (resp.magic != XWL_SCREENCAST_HELPER_MAGIC ||
        resp.version != XWL_SCREENCAST_HELPER_VERSION) {
        xwl_screencast_log("Xwayland screenshot portal: helper request id=%u "
                           "invalid response header magic=0x%x version=%u "
                           "elapsed=%" PRId64 "ms",
                           request_id, resp.magic, resp.version,
                           xwl_screencast_monotonic_msec() - start_ms);
        goto out;
    }

    if (resp.status == XWL_SCREENCAST_HELPER_STATUS_NO_FRAME) {
        xwl_screencast_log("Xwayland screenshot portal: helper request id=%u "
                           "returned no frame portal_response=%u "
                           "elapsed=%" PRId64 "ms",
                           request_id, resp.reserved,
                           xwl_screencast_monotonic_msec() - start_ms);
        status = XWL_SCREENCAST_FRAME_NO_FRAME;
        goto out;
    }

    if (resp.status != XWL_SCREENCAST_HELPER_STATUS_OK) {
        xwl_screencast_log("Xwayland screenshot portal: helper request id=%u "
                           "returned error status=%u elapsed=%" PRId64 "ms",
                           request_id, resp.status,
                           xwl_screencast_monotonic_msec() - start_ms);
        goto out;
    }

    minimum_stride = (uint64_t) resp.frame_width * XWL_SCREENCAST_BPP;
    expected_size = (uint64_t) resp.frame_stride * resp.frame_height;
    if (resp.frame_width == 0 || resp.frame_width > INT_MAX ||
        resp.frame_height == 0 || resp.frame_height > INT_MAX ||
        resp.frame_stride > INT_MAX ||
        resp.frame_stride < minimum_stride ||
        resp.frame_size != expected_size ||
        resp.frame_size == 0 || resp.frame_size > SIZE_MAX ||
        resp.frame_size > XWL_SCREENCAST_MAX_FRAME_SIZE ||
        (resp.format != SPA_VIDEO_FORMAT_BGRx &&
         resp.format != SPA_VIDEO_FORMAT_BGRA &&
         resp.format != SPA_VIDEO_FORMAT_RGBx &&
         resp.format != SPA_VIDEO_FORMAT_RGBA)) {
        xwl_screencast_log("Xwayland screenshot portal: helper request id=%u "
                           "invalid response status=%u frame=%ux%u "
                           "stride=%u size=%" PRIu64 " elapsed=%" PRId64
                           "ms",
                           request_id, resp.status, resp.frame_width,
                           resp.frame_height, resp.frame_stride,
                           resp.frame_size,
                           xwl_screencast_monotonic_msec() - start_ms);
        goto out;
    }

    xwl_screencast_log("Xwayland screenshot portal: helper request id=%u "
                       "response ok frame=%ux%u stride=%u size=%" PRIu64
                       " elapsed=%" PRId64 "ms",
                       request_id, resp.frame_width, resp.frame_height,
                       resp.frame_stride, resp.frame_size,
                       xwl_screencast_monotonic_msec() - start_ms);

    frame = malloc(resp.frame_size);
    if (!frame)
        goto out;

    deadline_ms = xwl_screencast_monotonic_msec() +
                  XWL_SCREENCAST_HELPER_PAYLOAD_TIMEOUT_MS;
    if (!xwl_screencast_read_full_timed(fd, frame, resp.frame_size, deadline_ms)) {
        xwl_screencast_log("Xwayland screenshot portal: helper request id=%u "
                           "failed to read frame payload: %s elapsed=%" PRId64
                           "ms",
                           request_id, strerror(errno),
                           xwl_screencast_monotonic_msec() - start_ms);
        goto out;
    }

    xwl_screencast_log("Xwayland screenshot portal: helper request id=%u "
                       "received helper frame total=%" PRId64 "ms",
                       request_id, xwl_screencast_monotonic_msec() - start_ms);

    *response_out = resp;
    *frame_out = frame;
    frame = NULL;
    status = XWL_SCREENCAST_FRAME_OK;

out:
    free(frame);
    if (fd >= 0)
        xwl_screencast_pending_close_fd(pending, fd);
    return status;
}

static Bool
append_dict_entry_uint32(DBusMessageIter *dict, const char *key, uint32_t value)
{
    DBusMessageIter entry, variant;
    const char *key_arg = key;

    if (!dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry))
        return FALSE;
    if (!dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key_arg))
        return FALSE;
    if (!dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "u", &variant))
        return FALSE;
    if (!dbus_message_iter_append_basic(&variant, DBUS_TYPE_UINT32, &value))
        return FALSE;
    if (!dbus_message_iter_close_container(&entry, &variant))
        return FALSE;
    if (!dbus_message_iter_close_container(dict, &entry))
        return FALSE;

    return TRUE;
}

static Bool
append_dict_entry_bool(DBusMessageIter *dict, const char *key, dbus_bool_t value)
{
    DBusMessageIter entry, variant;
    const char *key_arg = key;

    if (!dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry))
        return FALSE;
    if (!dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key_arg))
        return FALSE;
    if (!dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "b", &variant))
        return FALSE;
    if (!dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &value))
        return FALSE;
    if (!dbus_message_iter_close_container(&entry, &variant))
        return FALSE;
    if (!dbus_message_iter_close_container(dict, &entry))
        return FALSE;

    return TRUE;
}

static Bool
append_dict_entry_string(DBusMessageIter *dict, const char *key, const char *value)
{
    DBusMessageIter entry, variant;
    const char *key_arg = key;
    const char *value_arg = value;

    if (!dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry))
        return FALSE;
    if (!dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key_arg))
        return FALSE;
    if (!dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &variant))
        return FALSE;
    if (!dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &value_arg))
        return FALSE;
    if (!dbus_message_iter_close_container(&entry, &variant))
        return FALSE;
    if (!dbus_message_iter_close_container(dict, &entry))
        return FALSE;

    return TRUE;
}

static DBusMessage *
portal_call_with_timeout(DBusConnection *connection,
                         DBusMessage *message,
                         int timeout_ms)
{
    DBusError error;
    DBusMessage *reply;

    dbus_error_init(&error);
    reply = dbus_connection_send_with_reply_and_block(connection, message,
                                                      timeout_ms, &error);
    dbus_message_unref(message);

    if (dbus_error_is_set(&error)) {
        xwl_screencast_log("Xwayland screencast portal D-Bus error: %s",
                           error.message);
        dbus_error_free(&error);
    }

    return reply;
}

static DBusMessage *
portal_call(DBusConnection *connection, DBusMessage *message)
{
    return portal_call_with_timeout(connection, message,
                                    XWL_SCREENCAST_DBUS_CALL_TIMEOUT_MS);
}

static DBusConnection *
portal_open_session_bus_address(const char *address)
{
    DBusConnection *connection;
    DBusError error;

    dbus_error_init(&error);
    connection = dbus_connection_open_private(address, &error);
    if (!connection) {
        if (dbus_error_is_set(&error)) {
            xwl_screencast_log("Xwayland screencast portal D-Bus direct "
                               "connection failed for %s: %s",
                               address, error.message);
            dbus_error_free(&error);
        }
        return NULL;
    }

    if (!dbus_bus_register(connection, &error)) {
        if (dbus_error_is_set(&error)) {
            xwl_screencast_log("Xwayland screencast portal D-Bus register "
                               "failed for %s: %s", address, error.message);
            dbus_error_free(&error);
        }
        dbus_connection_close(connection);
        dbus_connection_unref(connection);
        return NULL;
    }

    return connection;
}

static DBusConnection *
portal_open_session_bus(struct xwl_screencast *screencast)
{
    DBusConnection *connection;
    DBusError error;
    char *preferred_address = NULL;
    pid_t preferred_pid = -1;
    uid_t preferred_uid = 0;
    Bool preferred_uid_valid = FALSE;
    const char *runtime_dir;
    const char *env_bus_address;
    char runtime_address[PATH_MAX + 32];
    char user_address[PATH_MAX + 32];

    pthread_mutex_lock(&screencast->mutex);
    if (screencast->bus_address) {
        preferred_address = strdup(screencast->bus_address);
        preferred_pid = screencast->bus_pid;
        preferred_uid = screencast->bus_uid;
        preferred_uid_valid = screencast->bus_uid_valid;
    }
    pthread_mutex_unlock(&screencast->mutex);

    if (preferred_address) {
        connection = portal_open_session_bus_address(preferred_address);
        free(preferred_address);
        if (connection)
            return connection;

        if (preferred_uid_valid && preferred_uid != geteuid()) {
            xwl_screencast_log("Xwayland screencast portal: selected session "
                               "bus belongs to client pid=%ld uid=%ld, but "
                               "Xwayland euid=%ld; path-based user buses "
                               "under /run/user are normally inaccessible "
                               "across this uid boundary",
                               (long) preferred_pid,
                               (long) preferred_uid,
                               (long) geteuid());
        }
    }

    env_bus_address = getenv("DBUS_SESSION_BUS_ADDRESS");
    if (env_bus_address && env_bus_address[0] != '\0') {
        connection = portal_open_session_bus_address(env_bus_address);
        if (connection)
            return connection;
    }

    dbus_error_init(&error);
    connection = dbus_bus_get_private(DBUS_BUS_SESSION, &error);
    if (connection)
        return connection;

    if (dbus_error_is_set(&error)) {
        xwl_screencast_log("Xwayland screencast portal D-Bus session bus "
                           "lookup failed: %s", error.message);
        dbus_error_free(&error);
    }

    runtime_dir = getenv("XDG_RUNTIME_DIR");
    if (runtime_dir && runtime_dir[0] != '\0') {
        snprintf(runtime_address, sizeof runtime_address,
                 "unix:path=%s/bus", runtime_dir);
        connection = portal_open_session_bus_address(runtime_address);
        if (connection)
            return connection;
    }

    snprintf(user_address, sizeof user_address, "unix:path=/run/user/%lu/bus",
             (unsigned long) getuid());
    if (!runtime_dir || runtime_dir[0] == '\0' ||
        strcmp(user_address, runtime_address) != 0)
        return portal_open_session_bus_address(user_address);

    return NULL;
}

static char *
portal_add_response_match(DBusConnection *connection, const char *path)
{
    DBusError error;
    char *rule;
    int len;

    if (path)
        len = snprintf(NULL, 0,
                       "type='signal',interface='%s',member='Response',path='%s'",
                       PORTAL_REQUEST_IFACE, path);
    else
        len = snprintf(NULL, 0,
                       "type='signal',interface='%s',member='Response'",
                       PORTAL_REQUEST_IFACE);
    rule = malloc(len + 1);
    if (!rule)
        return NULL;

    if (path)
        snprintf(rule, len + 1,
                 "type='signal',interface='%s',member='Response',path='%s'",
                 PORTAL_REQUEST_IFACE, path);
    else
        snprintf(rule, len + 1,
                 "type='signal',interface='%s',member='Response'",
                 PORTAL_REQUEST_IFACE);

    dbus_error_init(&error);
    dbus_bus_add_match(connection, rule, &error);
    dbus_connection_flush(connection);
    if (dbus_error_is_set(&error)) {
        xwl_screencast_log("Xwayland screencast portal D-Bus match error: %s",
                           error.message);
        dbus_error_free(&error);
        free(rule);
        return NULL;
    }

    return rule;
}

static DBusMessage *
portal_wait_response(struct xwl_screencast *screencast, const char *path)
{
    DBusConnection *connection = screencast->connection;
    DBusMessage *message;

    while (!xwl_screencast_stopped(screencast)) {
        dbus_connection_read_write(connection, 250);

        while ((message = dbus_connection_pop_message(connection)) != NULL) {
            const char *message_path = dbus_message_get_path(message);

            if (dbus_message_is_signal(message, PORTAL_REQUEST_IFACE, "Response") &&
                message_path && strcmp(message_path, path) == 0)
                return message;

            dbus_message_unref(message);
        }
    }

    return NULL;
}

static Bool
portal_response_success(DBusMessage *message, DBusMessageIter *results)
{
    DBusMessageIter iter;
    uint32_t response;

    if (!dbus_message_iter_init(message, &iter))
        return FALSE;

    if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_UINT32)
        return FALSE;

    dbus_message_iter_get_basic(&iter, &response);
    if (response != 0) {
        xwl_screencast_log("Xwayland screencast portal: request returned response %u",
                           response);
        return FALSE;
    }

    if (!dbus_message_iter_next(&iter))
        return FALSE;
    if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY)
        return FALSE;

    *results = iter;
    return TRUE;
}

static char *
response_lookup_string(DBusMessageIter *results, const char *wanted_key)
{
    DBusMessageIter dict;

    dbus_message_iter_recurse(results, &dict);
    while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry, variant;
        const char *key;
        int value_type;

        dbus_message_iter_recurse(&dict, &entry);
        if (dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_STRING)
            goto next;
        dbus_message_iter_get_basic(&entry, &key);
        if (strcmp(key, wanted_key) != 0)
            goto next;

        if (!dbus_message_iter_next(&entry))
            goto next;
        if (dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_VARIANT)
            goto next;

        dbus_message_iter_recurse(&entry, &variant);
        value_type = dbus_message_iter_get_arg_type(&variant);
        if (value_type == DBUS_TYPE_STRING || value_type == DBUS_TYPE_OBJECT_PATH) {
            const char *value;

            dbus_message_iter_get_basic(&variant, &value);
            return strdup(value);
        }

next:
        dbus_message_iter_next(&dict);
    }

    return NULL;
}

static uint32_t
portal_get_available_cursor_modes(DBusConnection *connection)
{
    DBusMessage *message, *reply;
    DBusMessageIter iter, variant;
    const char *interface_name = PORTAL_SCREENCAST_IFACE;
    const char *property_name = "AvailableCursorModes";
    uint32_t modes = 0;

    message = dbus_message_new_method_call(PORTAL_BUS_NAME,
                                           PORTAL_OBJECT_PATH,
                                           PORTAL_PROPERTIES_IFACE,
                                           "Get");
    if (!message)
        return 0;

    dbus_message_iter_init_append(message, &iter);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &interface_name);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &property_name);

    reply = portal_call(connection, message);
    if (!reply)
        return 0;

    if (reply && dbus_message_iter_init(reply, &iter) &&
        dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_VARIANT) {
        dbus_message_iter_recurse(&iter, &variant);
        if (dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_UINT32)
            dbus_message_iter_get_basic(&variant, &modes);
    }

    dbus_message_unref(reply);

    return modes;
}

static char *
portal_create_session(struct xwl_screencast *screencast)
{
    DBusMessage *message, *reply, *response;
    DBusMessageIter iter, options, results;
    char handle_token[64], session_token[64];
    char *request_path = NULL;
    char *session_handle = NULL;

    xwl_screencast_make_token(handle_token, sizeof handle_token, "xwl_sc_create");
    xwl_screencast_make_token(session_token, sizeof session_token, "xwl_sc_session");

    message = dbus_message_new_method_call(PORTAL_BUS_NAME,
                                           PORTAL_OBJECT_PATH,
                                           PORTAL_SCREENCAST_IFACE,
                                           "CreateSession");
    if (!message)
        return NULL;

    dbus_message_iter_init_append(message, &iter);
    if (!dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &options))
        goto out_message;
    if (!append_dict_entry_string(&options, "handle_token", handle_token))
        goto out_message;
    if (!append_dict_entry_string(&options, "session_handle_token", session_token))
        goto out_message;
    if (!dbus_message_iter_close_container(&iter, &options))
        goto out_message;

    reply = portal_call(screencast->connection, message);
    message = NULL;
    if (!reply)
        return NULL;

    dbus_message_get_args(reply, NULL,
                          DBUS_TYPE_OBJECT_PATH, &request_path,
                          DBUS_TYPE_INVALID);
    if (!request_path) {
        dbus_message_unref(reply);
        return NULL;
    }

    response = portal_wait_response(screencast, request_path);
    dbus_message_unref(reply);
    if (!response)
        return NULL;

    if (portal_response_success(response, &results))
        session_handle = response_lookup_string(&results, "session_handle");

    dbus_message_unref(response);
    return session_handle;

out_message:
    if (message)
        dbus_message_unref(message);
    return NULL;
}

static Bool
portal_select_sources(struct xwl_screencast *screencast)
{
    DBusMessage *message, *reply, *response;
    DBusMessageIter iter, options, results;
    char handle_token[64];
    const char *session_handle = screencast->session_handle;
    char *request_path = NULL;
    uint32_t cursor_modes;
    Bool ok = FALSE;

    xwl_screencast_make_token(handle_token, sizeof handle_token, "xwl_sc_select");
    cursor_modes = portal_get_available_cursor_modes(screencast->connection);

    message = dbus_message_new_method_call(PORTAL_BUS_NAME,
                                           PORTAL_OBJECT_PATH,
                                           PORTAL_SCREENCAST_IFACE,
                                           "SelectSources");
    if (!message)
        return FALSE;

    dbus_message_iter_init_append(message, &iter);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &session_handle);
    if (!dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &options))
        goto out_message;
    if (!append_dict_entry_string(&options, "handle_token", handle_token))
        goto out_message;
    if (!append_dict_entry_uint32(&options, "types", PORTAL_SCREENCAST_SOURCE_MONITOR))
        goto out_message;
    if (!append_dict_entry_bool(&options, "multiple", TRUE))
        goto out_message;
    if (cursor_modes & PORTAL_SCREENCAST_CURSOR_EMBEDDED) {
        if (!append_dict_entry_uint32(&options, "cursor_mode",
                                      PORTAL_SCREENCAST_CURSOR_EMBEDDED))
            goto out_message;
    } else if (cursor_modes & PORTAL_SCREENCAST_CURSOR_HIDDEN) {
        if (!append_dict_entry_uint32(&options, "cursor_mode",
                                      PORTAL_SCREENCAST_CURSOR_HIDDEN))
            goto out_message;
    }
    if (!dbus_message_iter_close_container(&iter, &options))
        goto out_message;

    reply = portal_call(screencast->connection, message);
    message = NULL;
    if (!reply)
        return FALSE;

    dbus_message_get_args(reply, NULL,
                          DBUS_TYPE_OBJECT_PATH, &request_path,
                          DBUS_TYPE_INVALID);
    if (!request_path) {
        dbus_message_unref(reply);
        return FALSE;
    }

    response = portal_wait_response(screencast, request_path);
    dbus_message_unref(reply);
    if (!response)
        return FALSE;

    ok = portal_response_success(response, &results);
    dbus_message_unref(response);
    return ok;

out_message:
    if (message)
        dbus_message_unref(message);
    return FALSE;
}

static void
parse_stream_properties(DBusMessageIter *props,
                        struct xwl_screencast_stream *stream)
{
    DBusMessageIter dict;

    dbus_message_iter_recurse(props, &dict);
    while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry, variant, value;
        const char *key;

        dbus_message_iter_recurse(&dict, &entry);
        if (dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_STRING)
            goto next;
        dbus_message_iter_get_basic(&entry, &key);

        if (!dbus_message_iter_next(&entry) ||
            dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_VARIANT)
            goto next;

        dbus_message_iter_recurse(&entry, &variant);
        if (strcmp(key, "position") == 0 &&
            dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_STRUCT) {
            dbus_message_iter_recurse(&variant, &value);
            if (dbus_message_iter_get_arg_type(&value) == DBUS_TYPE_INT32) {
                dbus_message_iter_get_basic(&value, &stream->x);
                if (dbus_message_iter_next(&value) &&
                    dbus_message_iter_get_arg_type(&value) == DBUS_TYPE_INT32)
                    dbus_message_iter_get_basic(&value, &stream->y);
            }
        } else if (strcmp(key, "size") == 0 &&
                   dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_STRUCT) {
            dbus_message_iter_recurse(&variant, &value);
            if (dbus_message_iter_get_arg_type(&value) == DBUS_TYPE_INT32) {
                dbus_message_iter_get_basic(&value, &stream->width);
                if (dbus_message_iter_next(&value) &&
                    dbus_message_iter_get_arg_type(&value) == DBUS_TYPE_INT32)
                    dbus_message_iter_get_basic(&value, &stream->height);
            }
        } else if (strcmp(key, "pipewire-serial") == 0) {
            if (dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_UINT64) {
                dbus_message_iter_get_basic(&variant, &stream->pipewire_serial);
            } else if (dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_UINT32) {
                uint32_t serial32;

                dbus_message_iter_get_basic(&variant, &serial32);
                stream->pipewire_serial = serial32;
            }
        }

next:
        dbus_message_iter_next(&dict);
    }
}

static Bool
parse_streams(DBusMessageIter *results, struct xwl_screencast *screencast)
{
    DBusMessageIter dict;

    dbus_message_iter_recurse(results, &dict);
    while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry, variant, array;
        const char *key;

        dbus_message_iter_recurse(&dict, &entry);
        if (dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_STRING)
            goto next;
        dbus_message_iter_get_basic(&entry, &key);
        if (strcmp(key, "streams") != 0)
            goto next;

        if (!dbus_message_iter_next(&entry) ||
            dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_VARIANT)
            goto next;

        dbus_message_iter_recurse(&entry, &variant);
        if (dbus_message_iter_get_arg_type(&variant) != DBUS_TYPE_ARRAY)
            goto next;

        dbus_message_iter_recurse(&variant, &array);
        while (dbus_message_iter_get_arg_type(&array) == DBUS_TYPE_STRUCT) {
            DBusMessageIter tuple;
            struct xwl_screencast_stream *streams;
            struct xwl_screencast_stream *stream;
            uint32_t node_id;

            dbus_message_iter_recurse(&array, &tuple);
            if (dbus_message_iter_get_arg_type(&tuple) != DBUS_TYPE_UINT32)
                goto stream_next;
            dbus_message_iter_get_basic(&tuple, &node_id);

            streams = reallocarray(screencast->streams,
                                   screencast->n_streams + 1,
                                   sizeof *screencast->streams);
            if (!streams)
                return FALSE;

            screencast->streams = streams;
            stream = &screencast->streams[screencast->n_streams++];
            memset(stream, 0, sizeof *stream);
            stream->screencast = screencast;
            stream->node_id = node_id;

            if (dbus_message_iter_next(&tuple) &&
                dbus_message_iter_get_arg_type(&tuple) == DBUS_TYPE_ARRAY)
                parse_stream_properties(&tuple, stream);

stream_next:
            dbus_message_iter_next(&array);
        }

        return screencast->n_streams > 0;

next:
        dbus_message_iter_next(&dict);
    }

    return FALSE;
}

static Bool
portal_start(struct xwl_screencast *screencast)
{
    DBusMessage *message, *reply, *response;
    DBusMessageIter iter, options, results;
    char handle_token[64];
    const char *session_handle = screencast->session_handle;
    const char *parent_window = "";
    char *request_path = NULL;
    Bool ok = FALSE;

    xwl_screencast_make_token(handle_token, sizeof handle_token, "xwl_sc_start");

    message = dbus_message_new_method_call(PORTAL_BUS_NAME,
                                           PORTAL_OBJECT_PATH,
                                           PORTAL_SCREENCAST_IFACE,
                                           "Start");
    if (!message)
        return FALSE;

    dbus_message_iter_init_append(message, &iter);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &session_handle);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &parent_window);
    if (!dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &options))
        goto out_message;
    if (!append_dict_entry_string(&options, "handle_token", handle_token))
        goto out_message;
    if (!dbus_message_iter_close_container(&iter, &options))
        goto out_message;

    reply = portal_call(screencast->connection, message);
    message = NULL;
    if (!reply)
        return FALSE;

    dbus_message_get_args(reply, NULL,
                          DBUS_TYPE_OBJECT_PATH, &request_path,
                          DBUS_TYPE_INVALID);
    if (!request_path) {
        dbus_message_unref(reply);
        return FALSE;
    }

    response = portal_wait_response(screencast, request_path);
    dbus_message_unref(reply);
    if (!response)
        return FALSE;

    if (portal_response_success(response, &results))
        ok = parse_streams(&results, screencast);

    dbus_message_unref(response);
    return ok;

out_message:
    if (message)
        dbus_message_unref(message);
    return FALSE;
}

static int
portal_open_pipewire_remote(struct xwl_screencast *screencast)
{
    DBusMessage *message, *reply;
    DBusMessageIter iter, options;
    const char *session_handle = screencast->session_handle;
    int fd = -1;

    message = dbus_message_new_method_call(PORTAL_BUS_NAME,
                                           PORTAL_OBJECT_PATH,
                                           PORTAL_SCREENCAST_IFACE,
                                           "OpenPipeWireRemote");
    if (!message)
        return -1;

    dbus_message_iter_init_append(message, &iter);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &session_handle);
    if (!dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &options))
        goto out_message;
    if (!dbus_message_iter_close_container(&iter, &options))
        goto out_message;

    reply = portal_call(screencast->connection, message);
    message = NULL;
    if (!reply)
        return -1;

    dbus_message_get_args(reply, NULL,
                          DBUS_TYPE_UNIX_FD, &fd,
                          DBUS_TYPE_INVALID);
    dbus_message_unref(reply);

    return fd;

out_message:
    if (message)
        dbus_message_unref(message);
    return -1;
}

static void
stream_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
    struct xwl_screencast_stream *stream = data;

    if (!param || id != SPA_PARAM_Format)
        return;

    spa_format_video_raw_parse(param, &stream->raw);
}

static void
stream_state_changed(void *data, enum pw_stream_state old,
                     enum pw_stream_state state, const char *error)
{
    struct xwl_screencast_stream *stream = data;

    if (error || state == PW_STREAM_STATE_ERROR) {
        xwl_screencast_log("Xwayland screencast portal: stream %u state "
                           "%s -> %s%s%s",
                           stream->node_id,
                           pw_stream_state_as_string(old),
                           pw_stream_state_as_string(state),
                           error ? ": " : "",
                           error ? error : "");
    }
}

static void
stream_process(void *data)
{
    struct xwl_screencast_stream *stream = data;
    struct xwl_screencast *screencast = stream->screencast;
    struct pw_buffer *buffer;
    struct spa_buffer *spa_buffer;
    struct spa_data *spa_data;
    struct spa_chunk *chunk;
    int width, height, stride;
    size_t alloc_size;
    size_t available;
    size_t needed;
    size_t offset;
    size_t row_bytes;
    size_t stride_size;
    uint64_t fd_offset;
    size_t y;
    uint8_t *frame;

    buffer = pw_stream_dequeue_buffer(stream->pw_stream);
    if (!buffer)
        return;

    spa_buffer = buffer->buffer;
    if (!spa_buffer || spa_buffer->n_datas < 1)
        goto out;

    spa_data = &spa_buffer->datas[0];
    chunk = spa_data->chunk;
    if (!chunk)
        goto out;

    if (spa_data->type != SPA_DATA_MemFd || spa_data->fd < 0) {
        if (!stream->logged_unsupported_buffer) {
            xwl_screencast_log("Xwayland screencast portal: ignoring "
                               "unsupported PipeWire buffer type=%u "
                               "fd=%" PRId64,
                               spa_data->type, spa_data->fd);
            stream->logged_unsupported_buffer = TRUE;
        }
        goto out;
    }

    width = stream->raw.size.width;
    height = stream->raw.size.height;
    stride = chunk->stride;
    if (width <= 0 || height <= 0)
        goto out;
    if (stride == 0)
        stride = width * XWL_SCREENCAST_BPP;
    if (stride < 0)
        goto out;

    row_bytes = (size_t) width * XWL_SCREENCAST_BPP;
    stride_size = (size_t) stride;
    if (stride_size < row_bytes)
        goto out;
    if (spa_data->maxsize == 0)
        goto out;
    if ((size_t) height > SIZE_MAX / stride_size)
        goto out;
    if ((size_t) (height - 1) > (SIZE_MAX - row_bytes) / stride_size)
        goto out;

    alloc_size = stride_size * (size_t) height;
    needed = stride_size * (size_t) (height - 1) + row_bytes;
    offset = chunk->offset % spa_data->maxsize;
    available = spa_data->maxsize - offset;
    if (chunk->size > 0 && chunk->size < available)
        available = chunk->size;
    if (available < needed) {
        if (!stream->logged_short_buffer) {
            xwl_screencast_log("Xwayland screencast portal: ignoring short "
                               "PipeWire buffer available=%zu needed=%zu "
                               "offset=%zu maxsize=%u chunk-size=%u stride=%d "
                               "height=%d",
                               available, needed, offset, spa_data->maxsize,
                               chunk->size, stride, height);
            stream->logged_short_buffer = TRUE;
        }
        goto out;
    }

    frame = calloc(1, alloc_size);
    if (!frame)
        goto out;

    fd_offset = (uint64_t) spa_data->mapoffset + offset;
    if (fd_offset > INT64_MAX ||
        (uint64_t) (height - 1) >
        ((uint64_t) INT64_MAX - fd_offset) / stride_size) {
        free(frame);
        goto out;
    }

    for (y = 0; y < (size_t) height; y++) {
        if (!xwl_screencast_pread_full(spa_data->fd,
                                       frame + y * stride_size,
                                       row_bytes,
                                       (off_t) (fd_offset +
                                                y * stride_size))) {
            xwl_screencast_log("Xwayland screencast portal: failed to "
                               "read PipeWire memfd buffer: %s",
                               strerror(errno));
            free(frame);
            goto out;
        }
    }

    pthread_mutex_lock(&screencast->mutex);
    free(stream->frame);
    stream->frame = frame;
    stream->frame_size = alloc_size;
    stream->frame_width = width;
    stream->frame_height = height;
    stream->frame_stride = stride;
    stream->frame_format = stream->raw.format;
    stream->frame_valid = TRUE;
    if (stream->width == 0)
        stream->width = width;
    if (stream->height == 0)
        stream->height = height;
    xwl_screencast_signal_frame(screencast);
    pthread_mutex_unlock(&screencast->mutex);

out:
    pw_stream_queue_buffer(stream->pw_stream, buffer);
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = stream_state_changed,
    .param_changed = stream_param_changed,
    .process = stream_process,
};

static const struct spa_pod *
build_format(struct spa_pod_builder *builder, enum spa_video_format format)
{
    struct spa_rectangle default_size = SPA_RECTANGLE(1920, 1080);
    struct spa_rectangle min_size = SPA_RECTANGLE(1, 1);
    struct spa_rectangle max_size = SPA_RECTANGLE(16384, 16384);
    struct spa_fraction default_rate = SPA_FRACTION(30, 1);
    struct spa_fraction min_rate = SPA_FRACTION(0, 1);
    struct spa_fraction max_rate = SPA_FRACTION(120, 1);

    return spa_pod_builder_add_object(builder,
                                      SPA_TYPE_OBJECT_Format,
                                      SPA_PARAM_EnumFormat,
                                      SPA_FORMAT_mediaType,
                                      SPA_POD_Id(SPA_MEDIA_TYPE_video),
                                      SPA_FORMAT_mediaSubtype,
                                      SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                                      SPA_FORMAT_VIDEO_format,
                                      SPA_POD_Id(format),
                                      SPA_FORMAT_VIDEO_size,
                                      SPA_POD_CHOICE_RANGE_Rectangle(&default_size,
                                                                     &min_size,
                                                                     &max_size),
                                      SPA_FORMAT_VIDEO_framerate,
                                      SPA_POD_CHOICE_RANGE_Fraction(&default_rate,
                                                                    &min_rate,
                                                                    &max_rate));
}

static const struct spa_pod *
build_buffers(struct spa_pod_builder *builder)
{
    return spa_pod_builder_add_object(builder,
                                      SPA_TYPE_OBJECT_ParamBuffers,
                                      SPA_PARAM_Buffers,
                                      SPA_PARAM_BUFFERS_buffers,
                                      SPA_POD_CHOICE_RANGE_Int(8, 2, 16),
                                      SPA_PARAM_BUFFERS_blocks,
                                      SPA_POD_Int(1),
                                      SPA_PARAM_BUFFERS_align,
                                      SPA_POD_Int(16),
                                      SPA_PARAM_BUFFERS_dataType,
                                      SPA_POD_CHOICE_FLAGS_Int(XWL_SCREENCAST_BUFFER_TYPES));
}

static Bool
pipewire_connect_streams(struct xwl_screencast *screencast, int fd)
{
    uint32_t i;
    Bool ok = FALSE;

    pw_init(NULL, NULL);

    screencast->pw_loop = pw_thread_loop_new("xwayland-screencast", NULL);
    if (!screencast->pw_loop)
        return FALSE;

    screencast->pw_context =
        pw_context_new(pw_thread_loop_get_loop(screencast->pw_loop), NULL, 0);
    if (!screencast->pw_context)
        return FALSE;

    screencast->pw_core = pw_context_connect_fd(screencast->pw_context, fd, NULL, 0);
    if (!screencast->pw_core)
        return FALSE;

    if (pw_thread_loop_start(screencast->pw_loop) < 0)
        return FALSE;

    pw_thread_loop_lock(screencast->pw_loop);

    for (i = 0; i < screencast->n_streams; i++) {
        struct xwl_screencast_stream *stream = &screencast->streams[i];
        struct pw_properties *props;
        const struct spa_pod *params[5];
        uint8_t buffer[4096];
        struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof buffer);
        uint32_t target_id = stream->node_id;
        char target_object[32];
        int ret;

        props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Video",
                                  PW_KEY_MEDIA_CATEGORY, "Capture",
                                  PW_KEY_MEDIA_ROLE, "Screen",
                                  NULL);
        if (stream->pipewire_serial) {
            snprintf(target_object, sizeof target_object, "%" PRIu64,
                     stream->pipewire_serial);
            pw_properties_set(props, PW_KEY_TARGET_OBJECT, target_object);
            target_id = PW_ID_ANY;
        }

        stream->pw_stream = pw_stream_new(screencast->pw_core,
                                          "xwayland-screencast",
                                          props);
        if (!stream->pw_stream)
            goto out_unlock;

        pw_stream_add_listener(stream->pw_stream, &stream->stream_listener,
                               &stream_events, stream);

        params[0] = build_format(&builder, SPA_VIDEO_FORMAT_BGRx);
        params[1] = build_format(&builder, SPA_VIDEO_FORMAT_BGRA);
        params[2] = build_format(&builder, SPA_VIDEO_FORMAT_RGBx);
        params[3] = build_format(&builder, SPA_VIDEO_FORMAT_RGBA);
        params[4] = build_buffers(&builder);

        ret = pw_stream_connect(stream->pw_stream,
                                PW_DIRECTION_INPUT,
                                target_id,
                                PW_STREAM_FLAG_AUTOCONNECT |
                                PW_STREAM_FLAG_MAP_BUFFERS,
                                params, ARRAY_SIZE(params));
        if (ret < 0) {
            xwl_screencast_log("Xwayland screencast portal: failed to connect "
                               "PipeWire stream %u: %s",
                               stream->node_id, spa_strerror(ret));
            goto out_unlock;
        }

        pw_stream_set_active(stream->pw_stream, true);
    }

    ok = TRUE;

out_unlock:
    pw_thread_loop_unlock(screencast->pw_loop);
    return ok;
}

static void
pipewire_cleanup(struct xwl_screencast *screencast)
{
    uint32_t i;

    if (screencast->pw_loop)
        pw_thread_loop_stop(screencast->pw_loop);

    for (i = 0; i < screencast->n_streams; i++) {
        struct xwl_screencast_stream *stream = &screencast->streams[i];

        if (stream->pw_stream)
            pw_stream_destroy(stream->pw_stream);
        free(stream->frame);
    }

    if (screencast->pw_core)
        pw_core_disconnect(screencast->pw_core);
    if (screencast->pw_context)
        pw_context_destroy(screencast->pw_context);
    if (screencast->pw_loop)
        pw_thread_loop_destroy(screencast->pw_loop);

    free(screencast->streams);
    screencast->streams = NULL;
    screencast->n_streams = 0;
}

static Bool
portal_connect(struct xwl_screencast *screencast)
{
    int fd;

    xwl_screencast_set_stage(screencast, "connecting to session bus");
    screencast->connection = portal_open_session_bus(screencast);
    if (!screencast->connection)
        return FALSE;

    dbus_connection_set_exit_on_disconnect(screencast->connection, FALSE);
    if (xwl_screencast_stopped(screencast))
        return FALSE;

    xwl_screencast_set_stage(screencast, "subscribing to portal responses");
    screencast->response_match =
        portal_add_response_match(screencast->connection, NULL);
    if (!screencast->response_match) {
        xwl_screencast_error("failed to subscribe to portal responses");
        return FALSE;
    }
    if (xwl_screencast_stopped(screencast))
        return FALSE;

    xwl_screencast_set_stage(screencast, "creating portal session");
    screencast->session_handle = portal_create_session(screencast);
    if (!screencast->session_handle) {
        xwl_screencast_error("failed to create portal session");
        return FALSE;
    }
    if (xwl_screencast_stopped(screencast))
        return FALSE;

    xwl_screencast_set_stage(screencast, "selecting portal sources");
    if (!portal_select_sources(screencast)) {
        xwl_screencast_error("failed to select portal sources");
        return FALSE;
    }
    if (xwl_screencast_stopped(screencast))
        return FALSE;

    xwl_screencast_set_stage(screencast, "starting portal session");
    if (!portal_start(screencast)) {
        xwl_screencast_error("failed to start portal session");
        return FALSE;
    }
    if (xwl_screencast_stopped(screencast))
        return FALSE;

    xwl_screencast_set_stage(screencast, "opening PipeWire remote");
    fd = portal_open_pipewire_remote(screencast);
    if (fd < 0) {
        xwl_screencast_error("failed to open PipeWire remote");
        return FALSE;
    }
    if (xwl_screencast_stopped(screencast)) {
        close(fd);
        return FALSE;
    }

    xwl_screencast_set_stage(screencast, "connecting PipeWire streams");
    if (!pipewire_connect_streams(screencast, fd)) {
        xwl_screencast_error("failed to connect PipeWire streams");
        return FALSE;
    }
    if (xwl_screencast_stopped(screencast))
        return FALSE;

    xwl_screencast_set_stage(screencast, "waiting for PipeWire frames");

    return TRUE;
}

static void
portal_cleanup(struct xwl_screencast *screencast)
{
    if (screencast->connection && screencast->session_handle) {
        DBusMessage *message;
        const char *session_handle = screencast->session_handle;

        message = dbus_message_new_method_call(PORTAL_BUS_NAME,
                                               session_handle,
                                               "org.freedesktop.portal.Session",
                                               "Close");
        if (message) {
            DBusMessage *reply =
                portal_call_with_timeout(screencast->connection, message,
                                         XWL_SCREENCAST_DBUS_CLOSE_TIMEOUT_MS);

            if (reply)
                dbus_message_unref(reply);
        }
    }

    free(screencast->session_handle);
    screencast->session_handle = NULL;

    if (screencast->connection) {
        if (screencast->response_match) {
            dbus_bus_remove_match(screencast->connection,
                                  screencast->response_match, NULL);
            free(screencast->response_match);
            screencast->response_match = NULL;
        }

        dbus_connection_close(screencast->connection);
        dbus_connection_unref(screencast->connection);
        screencast->connection = NULL;
    }
}

static void *
xwl_screencast_thread(void *data)
{
    struct xwl_screencast *screencast = data;

    if (!portal_connect(screencast)) {
        xwl_screencast_set_failed(screencast);
        goto out;
    }

    pthread_mutex_lock(&screencast->mutex);
    while (!screencast->stop)
        pthread_cond_wait(&screencast->cond, &screencast->mutex);
    pthread_mutex_unlock(&screencast->mutex);

out:
    pipewire_cleanup(screencast);
    portal_cleanup(screencast);
    pthread_mutex_lock(&screencast->mutex);
    screencast->thread_finished = TRUE;
    pthread_cond_broadcast(&screencast->cond);
    pthread_mutex_unlock(&screencast->mutex);
    return NULL;
}

static Bool
xwl_screencast_start_locked(struct xwl_screencast *screencast)
{
    if (screencast->started)
        return TRUE;

    screencast->started = TRUE;
    screencast->thread_finished = FALSE;
    if (pthread_create(&screencast->thread, NULL,
                       xwl_screencast_thread, screencast) != 0) {
        screencast->started = FALSE;
        screencast->failed = TRUE;
        pthread_cond_broadcast(&screencast->cond);
        return FALSE;
    }

    return TRUE;
}

static void
timespec_add_ms(struct timespec *ts, int ms)
{
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (long) (ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
}

static Bool
xwl_screencast_helper_request_key(ClientPtr client,
                                  uint64_t *client_serial,
                                  int *request_sequence)
{
    struct xwl_screencast_client *sc_client;

    if (!client)
        return FALSE;

    sc_client = dixLookupPrivate(&client->devPrivates,
                                 &xwl_screencast_client_private_key);
    if (!sc_client || sc_client->serial == 0)
        return FALSE;

    *client_serial = sc_client->serial;
    *request_sequence = client->sequence;
    return TRUE;
}

static Bool
xwl_screencast_cached_helper_status(
    struct xwl_screencast *screencast,
    ClientPtr client,
    enum xwl_screencast_frame_status *status)
{
    uint64_t client_serial;
    int request_sequence;
    Bool matches;

    if (!xwl_screencast_helper_request_key(client, &client_serial,
                                           &request_sequence))
        return FALSE;

    pthread_mutex_lock(&screencast->mutex);
    matches = screencast->helper_frame_key_valid &&
              screencast->helper_frame_client_serial == client_serial &&
              screencast->helper_frame_request_sequence == request_sequence;
    if (matches && screencast->helper_frame_status == XWL_SCREENCAST_FRAME_OK &&
        (screencast->n_streams == 0 ||
         !screencast->streams[0].frame_valid ||
         !screencast->streams[0].frame)) {
        screencast->helper_frame_key_valid = FALSE;
        matches = FALSE;
    }
    if (matches)
        *status = screencast->helper_frame_status;
    pthread_mutex_unlock(&screencast->mutex);

    return matches;
}

static void
xwl_screencast_cache_helper_status(
    struct xwl_screencast *screencast,
    ClientPtr client,
    enum xwl_screencast_frame_status status)
{
    uint64_t client_serial;
    int request_sequence;

    if (!xwl_screencast_helper_request_key(client, &client_serial,
                                           &request_sequence))
        return;

    pthread_mutex_lock(&screencast->mutex);
    screencast->helper_frame_client_serial = client_serial;
    screencast->helper_frame_request_sequence = request_sequence;
    screencast->helper_frame_status = status;
    screencast->helper_frame_key_valid = TRUE;
    pthread_mutex_unlock(&screencast->mutex);
}

static enum xwl_screencast_frame_status
xwl_screencast_ensure_frame(struct xwl_screencast *screencast,
                            ClientPtr client)
{
    struct timespec deadline;
    uid_t helper_uid = 0;
    uid_t current_client_uid = 0;
    Bool use_helper = FALSE;
    Bool current_client_uid_valid = FALSE;
    pid_t current_client_pid = -1;
    enum xwl_screencast_frame_status status;
    Bool ok;

    /* Preparation failures are cached as well as successful frames.  Honor
     * them before choosing a transport so an unavailable client identity (or
     * helper) cannot fall through to the legacy synchronous portal path. */
    if (xwl_screencast_cached_helper_status(screencast, client, &status))
        return status;

    xwl_screencast_cached_client_creds(client, &current_client_pid,
                                       &current_client_uid,
                                       &current_client_uid_valid);
    xwl_screencast_set_client_bus(screencast, client);

    pthread_mutex_lock(&screencast->mutex);
    if (current_client_uid_valid) {
        helper_uid = current_client_uid;
        use_helper = TRUE;
    } else if (screencast->bus_uid_valid) {
        helper_uid = screencast->bus_uid;
        use_helper = TRUE;
    }
    pthread_mutex_unlock(&screencast->mutex);

    if (use_helper) {
        Bool log_unavailable = FALSE;

        pthread_mutex_lock(&screencast->mutex);
        if (!screencast->logged_helper_unavailable) {
            screencast->logged_helper_unavailable = TRUE;
            log_unavailable = TRUE;
        }
        pthread_mutex_unlock(&screencast->mutex);

        if (log_unavailable) {
            xwl_screencast_log("Xwayland screencast portal helper: request "
                               "reached the image hook without an asynchronously "
                               "prepared frame for uid=%ld",
                               (long) helper_uid);
        }

        return XWL_SCREENCAST_FRAME_ERROR;
    }

    /* Every protocol request reaches this function with its ClientPtr.  A
     * request that bypassed asynchronous preparation must remain unhandled;
     * running the legacy portal session here would block the Xserver thread. */
    if (client)
        return XWL_SCREENCAST_FRAME_ERROR;

    pthread_mutex_lock(&screencast->mutex);

    if (screencast->failed && screencast->thread_finished &&
        screencast->started) {
        pthread_t thread = screencast->thread;

        pthread_mutex_unlock(&screencast->mutex);
        pthread_join(thread, NULL);
        pthread_mutex_lock(&screencast->mutex);

        screencast->started = FALSE;
        screencast->thread_finished = FALSE;
        screencast->failed = FALSE;
        screencast->have_frame = FALSE;
        screencast->logged_no_frame = FALSE;
        screencast->logged_no_overlap = FALSE;
        screencast->stage = "retrying after failed portal session";
        xwl_screencast_log("Xwayland screencast portal: retrying after "
                           "failed portal session");
    }

    if (!xwl_screencast_start_locked(screencast)) {
        pthread_mutex_unlock(&screencast->mutex);
        return XWL_SCREENCAST_FRAME_ERROR;
    }

    clock_gettime(CLOCK_REALTIME, &deadline);
    timespec_add_ms(&deadline, XWL_SCREENCAST_START_TIMEOUT_MS);

    while (!screencast->have_frame && !screencast->failed && !screencast->stop) {
        if (pthread_cond_timedwait(&screencast->cond,
                                   &screencast->mutex,
                                   &deadline) == ETIMEDOUT)
            break;
    }

    ok = screencast->have_frame && !screencast->failed;
    if (!ok) {
        xwl_screencast_log("Xwayland screencast portal: ensure frame failed "
                           "started=%d finished=%d failed=%d stop=%d "
                           "have_frame=%d stage=%s",
                           screencast->started,
                           screencast->thread_finished,
                           screencast->failed,
                           screencast->stop,
                           screencast->have_frame,
                           screencast->stage ? screencast->stage : "unknown");
    }
    status = ok ? XWL_SCREENCAST_FRAME_OK :
                  (screencast->failed ? XWL_SCREENCAST_FRAME_ERROR :
                                        XWL_SCREENCAST_FRAME_NO_FRAME);
    pthread_mutex_unlock(&screencast->mutex);

    return status;
}

static Bool
xwl_screencast_is_root_image_drawable(struct xwl_screen *xwl_screen,
                                      DrawablePtr drawable)
{
    ScreenPtr screen = drawable->pScreen;

    if (!xwl_screen->rootless)
        return FALSE;

    if (drawable->type == DRAWABLE_WINDOW &&
        (WindowPtr) drawable == screen->root)
        return TRUE;

    if (drawable->type == DRAWABLE_PIXMAP && screen->GetScreenPixmap) {
        PixmapPtr screen_pixmap = screen->GetScreenPixmap(screen);

        if (screen_pixmap && drawable == &screen_pixmap->drawable)
            return TRUE;
    }

    return FALSE;
}

static void
xwl_screencast_pending_unlink_locked(
    struct xwl_screencast *screencast,
    struct xwl_screencast_pending *pending)
{
    struct xwl_screencast_pending **link = &screencast->pending;

    while (*link) {
        if (*link == pending) {
            *link = pending->next;
            pending->next = NULL;
            return;
        }
        link = &(*link)->next;
    }
}

static void
xwl_screencast_pending_free(struct xwl_screencast_pending *pending)
{
    if (pending->thread_started)
        pthread_join(pending->thread, NULL);

    free(pending->frame);
    free(pending->app_id);
    free(pending->app_name);
    free(pending);
}

static void
xwl_screencast_pending_destroy(struct xwl_screencast_pending *pending)
{
    struct xwl_screencast *screencast = pending->screencast;

    pthread_mutex_lock(&screencast->mutex);
    if (pending->client) {
        struct xwl_screencast_client *sc_client =
            dixLookupPrivate(&pending->client->devPrivates,
                             &xwl_screencast_client_private_key);

        if (sc_client && sc_client->pending == pending)
            sc_client->pending = NULL;
    }
    pending->client = NULL;
    pending->cancelled = TRUE;
    if (pending->fd >= 0)
        shutdown(pending->fd, SHUT_RDWR);
    xwl_screencast_pending_unlink_locked(screencast, pending);
    pthread_mutex_unlock(&screencast->mutex);

    xwl_screencast_pending_free(pending);
}

static void *
xwl_screencast_pending_thread(void *data)
{
    struct xwl_screencast_pending *pending = data;
    struct xwl_screencast *screencast = pending->screencast;
    struct xwl_screencast_helper_response response;
    uint8_t *frame = NULL;
    enum xwl_screencast_frame_status status;

    status = xwl_screencast_get_helper_frame(pending,
                                             pending->uid,
                                             pending->app_id,
                                             pending->app_name,
                                             &response,
                                             &frame);

    pthread_mutex_lock(&screencast->mutex);
    if (pending->cancelled || screencast->stop) {
        status = XWL_SCREENCAST_FRAME_ERROR;
        free(frame);
        frame = NULL;
    }
    pending->status = status;
    pending->response = response;
    pending->frame = frame;
    pending->completed = TRUE;
    pthread_mutex_unlock(&screencast->mutex);

    xwl_screencast_wake_main(screencast);
    return NULL;
}

Bool
xwl_screencast_prepare_image(struct xwl_screen *xwl_screen,
                             ClientPtr client,
                             DrawablePtr drawable)
{
    struct xwl_screencast *screencast = xwl_screen->screencast;
    struct xwl_screencast_client *sc_client;
    struct xwl_screencast_pending *pending;
    struct xwl_screencast_helper_response response;
    enum xwl_screencast_frame_status status;
    uint8_t *frame = NULL;
    uint64_t client_serial;
    int request_sequence;
    pid_t pid = -1;
    uid_t uid = 0;
    Bool uid_valid = FALSE;

    if (!screencast || !xwl_screen->enable_screencast_portal || !client ||
        !xwl_screencast_is_root_image_drawable(xwl_screen, drawable))
        return FALSE;

    if (!xwl_screencast_helper_request_key(client, &client_serial,
                                           &request_sequence))
        return FALSE;

    if (xwl_screencast_cached_helper_status(screencast, client, &status))
        return FALSE;

    sc_client = dixLookupPrivate(&client->devPrivates,
                                 &xwl_screencast_client_private_key);
    if (!sc_client)
        return FALSE;

    pthread_mutex_lock(&screencast->mutex);
    pending = sc_client->pending;
    if (pending) {
        if (pending->client_serial != client_serial ||
            pending->request_sequence != request_sequence) {
            pthread_mutex_unlock(&screencast->mutex);
            xwl_screencast_log("Xwayland screenshot portal: refusing to "
                               "replace an in-flight request for client=%d",
                               client->index);
            return FALSE;
        }

        if (!pending->completed) {
            pthread_mutex_unlock(&screencast->mutex);
            return TRUE;
        }

        status = pending->status;
        response = pending->response;
        frame = pending->frame;
        pending->frame = NULL;
        sc_client->pending = NULL;
        xwl_screencast_pending_unlink_locked(screencast, pending);
        pthread_mutex_unlock(&screencast->mutex);

        xwl_screencast_pending_free(pending);
        if (status == XWL_SCREENCAST_FRAME_OK) {
            if (!xwl_screencast_store_helper_frame(screencast,
                                                   &response, frame)) {
                free(frame);
                status = XWL_SCREENCAST_FRAME_ERROR;
            }
        } else {
            free(frame);
        }

        xwl_screencast_cache_helper_status(screencast, client, status);
        return FALSE;
    }
    pthread_mutex_unlock(&screencast->mutex);

    xwl_screencast_cached_client_creds(client, &pid, &uid, &uid_valid);
    xwl_screencast_set_client_bus(screencast, client);
    if (!uid_valid) {
        pthread_mutex_lock(&screencast->mutex);
        if (screencast->bus_uid_valid) {
            uid = screencast->bus_uid;
            uid_valid = TRUE;
        }
        pthread_mutex_unlock(&screencast->mutex);
    }

    if (!uid_valid) {
        xwl_screencast_cache_helper_status(screencast, client,
                                           XWL_SCREENCAST_FRAME_ERROR);
        return FALSE;
    }

    pending = calloc(1, sizeof *pending);
    if (!pending) {
        xwl_screencast_cache_helper_status(screencast, client,
                                           XWL_SCREENCAST_FRAME_ERROR);
        return FALSE;
    }

    pending->screencast = screencast;
    pending->client = client;
    pending->client_serial = client_serial;
    pending->request_sequence = request_sequence;
    pending->fd = -1;
    pending->uid = uid;
    pending->status = XWL_SCREENCAST_FRAME_ERROR;
    pending->app_id = xwl_screencast_client_app_id(xwl_screen, client, pid,
                                                   &pending->app_name);
    if (!pending->app_id)
        pending->app_id = strdup("xwayland.unknown");
    if (!pending->app_name)
        pending->app_name = strdup(pending->app_id ? pending->app_id :
                                                     "Xwayland Application");
    if (!pending->app_id || !pending->app_name) {
        xwl_screencast_pending_free(pending);
        xwl_screencast_cache_helper_status(screencast, client,
                                           XWL_SCREENCAST_FRAME_ERROR);
        return FALSE;
    }

    pthread_mutex_lock(&screencast->mutex);
    if (screencast->stop || sc_client->pending) {
        pthread_mutex_unlock(&screencast->mutex);
        xwl_screencast_pending_free(pending);
        return FALSE;
    }
    pending->next = screencast->pending;
    screencast->pending = pending;
    sc_client->pending = pending;
    if (pthread_create(&pending->thread, NULL,
                       xwl_screencast_pending_thread, pending) != 0) {
        sc_client->pending = NULL;
        xwl_screencast_pending_unlink_locked(screencast, pending);
        pthread_mutex_unlock(&screencast->mutex);
        xwl_screencast_pending_free(pending);
        xwl_screencast_cache_helper_status(screencast, client,
                                           XWL_SCREENCAST_FRAME_ERROR);
        return FALSE;
    }
    pending->thread_started = TRUE;
    pthread_mutex_unlock(&screencast->mutex);

    xwl_screencast_log("Xwayland screenshot portal: suspended preparation "
                       "started for client=%d sequence=%d app_id=%s",
                       client->index, request_sequence, pending->app_id);
    return TRUE;
}

void
xwl_screencast_cancel_image(struct xwl_screen *xwl_screen, ClientPtr client)
{
    struct xwl_screencast *screencast = xwl_screen->screencast;
    struct xwl_screencast_client *sc_client;
    struct xwl_screencast_pending *pending;

    if (!screencast || !client)
        return;

    sc_client = dixLookupPrivate(&client->devPrivates,
                                 &xwl_screencast_client_private_key);
    if (!sc_client)
        return;

    pthread_mutex_lock(&screencast->mutex);
    pending = sc_client->pending;
    if (pending) {
        sc_client->pending = NULL;
        pending->client = NULL;
        pending->cancelled = TRUE;
        if (pending->fd >= 0)
            shutdown(pending->fd, SHUT_RDWR);
    }
    pthread_mutex_unlock(&screencast->mutex);
}

static Bool
xwl_screencast_root_position_for_drawable(struct xwl_screen *xwl_screen,
                                          DrawablePtr drawable,
                                          int x, int y,
                                          int *root_x, int *root_y)
{
    if (!xwl_screencast_is_root_image_drawable(xwl_screen, drawable))
        return FALSE;

    *root_x = x;
    *root_y = y;
    return TRUE;
}

static void
source_pixel_rgb(const uint8_t *src, enum spa_video_format format,
                 uint8_t *r, uint8_t *g, uint8_t *b)
{
    switch (format) {
    case SPA_VIDEO_FORMAT_RGBx:
    case SPA_VIDEO_FORMAT_RGBA:
        *r = src[0];
        *g = src[1];
        *b = src[2];
        break;
    case SPA_VIDEO_FORMAT_BGRx:
    case SPA_VIDEO_FORMAT_BGRA:
    default:
        *b = src[0];
        *g = src[1];
        *r = src[2];
        break;
    }
}

static void
write_x_pixel(uint8_t *dst, int bits_per_pixel, unsigned long plane_mask,
              uint8_t r, uint8_t g, uint8_t b)
{
    uint32_t pixel = ((uint32_t) r << 16) | ((uint32_t) g << 8) | b;

    pixel &= plane_mask;

    switch (bits_per_pixel) {
    case 32:
        memcpy(dst, &pixel, 4);
        break;
    case 24:
        dst[0] = pixel & 0xff;
        dst[1] = (pixel >> 8) & 0xff;
        dst[2] = (pixel >> 16) & 0xff;
        break;
    case 16: {
        uint16_t rgb565 = (((uint16_t) r & 0xf8) << 8) |
                          (((uint16_t) g & 0xfc) << 3) |
                          ((uint16_t) b >> 3);

        rgb565 &= plane_mask;
        memcpy(dst, &rgb565, 2);
        break;
    }
    default:
        *dst = pixel & 0xff;
        break;
    }
}

static Bool
copy_stream_to_ximage(struct xwl_screencast_stream *stream,
                      int root_width, int root_height,
                      int stream_offset_x, int stream_offset_y,
                      int x, int y, int width, int height,
                      int dst_stride, int bits_per_pixel,
                      unsigned long plane_mask, uint8_t *dst)
{
    int stream_x = stream->x - stream_offset_x;
    int stream_y = stream->y - stream_offset_y;
    int stream_width = stream->width;
    int stream_height = stream->height;
    int x1, y1, x2, y2;
    int bytes_per_pixel = bits_per_pixel == 24 ? 3 : (bits_per_pixel + 7) / 8;
    int row, col;
    Bool copied = FALSE;

    if (!stream->frame_valid || !stream->frame)
        return FALSE;

    if (stream_width <= 0)
        stream_width = root_width;
    if (stream_height <= 0)
        stream_height = root_height;

    x1 = max(x, stream_x);
    y1 = max(y, stream_y);
    x2 = min(x + width, stream_x + stream_width);
    y2 = min(y + height, stream_y + stream_height);

    if (x1 >= x2 || y1 >= y2)
        return FALSE;

    for (row = y1; row < y2; row++) {
        uint8_t *dst_row = dst + (row - y) * dst_stride;
        int src_y = (int) (((int64_t) (row - stream_y) *
                            stream->frame_height) / stream_height);

        if (src_y < 0 || src_y >= stream->frame_height)
            continue;

        for (col = x1; col < x2; col++) {
            int src_x = (int) (((int64_t) (col - stream_x) *
                                stream->frame_width) / stream_width);
            const uint8_t *src;
            size_t src_offset;
            uint8_t r, g, b;

            if (src_x < 0 || src_x >= stream->frame_width)
                continue;

            src_offset = (size_t) src_y * stream->frame_stride +
                         (size_t) src_x * XWL_SCREENCAST_BPP;
            if (src_offset > stream->frame_size ||
                stream->frame_size - src_offset < XWL_SCREENCAST_BPP)
                continue;
            src = stream->frame + src_offset;

            source_pixel_rgb(src, stream->frame_format, &r, &g, &b);
            write_x_pixel(dst_row + (col - x) * bytes_per_pixel,
                          bits_per_pixel, plane_mask, r, g, b);
            copied = TRUE;
        }
    }

    return copied;
}

static Bool
copy_stream_scaled_to_ximage(struct xwl_screencast_stream *stream,
                             int width, int height,
                             int dst_stride, int bits_per_pixel,
                             unsigned long plane_mask, uint8_t *dst)
{
    int bytes_per_pixel = bits_per_pixel == 24 ? 3 : (bits_per_pixel + 7) / 8;
    int row, col;
    Bool copied = FALSE;

    if (!stream->frame_valid || !stream->frame)
        return FALSE;

    for (row = 0; row < height; row++) {
        uint8_t *dst_row = dst + row * dst_stride;
        int src_y = (int) (((int64_t) row * stream->frame_height) / height);

        if (src_y < 0 || src_y >= stream->frame_height)
            continue;

        for (col = 0; col < width; col++) {
            int src_x = (int) (((int64_t) col * stream->frame_width) / width);
            const uint8_t *src;
            size_t src_offset;
            uint8_t r, g, b;

            if (src_x < 0 || src_x >= stream->frame_width)
                continue;

            src_offset = (size_t) src_y * stream->frame_stride +
                         (size_t) src_x * XWL_SCREENCAST_BPP;
            if (src_offset > stream->frame_size ||
                stream->frame_size - src_offset < XWL_SCREENCAST_BPP)
                continue;
            src = stream->frame + src_offset;

            source_pixel_rgb(src, stream->frame_format, &r, &g, &b);
            write_x_pixel(dst_row + col * bytes_per_pixel,
                          bits_per_pixel, plane_mask, r, g, b);
            copied = TRUE;
        }
    }

    return copied;
}

Bool
xwl_screencast_init(struct xwl_screen *xwl_screen)
{
    struct xwl_screencast *screencast;

    if (!xwl_screencast_client_private_registered) {
        if (!dixRegisterPrivateKey(&xwl_screencast_client_private_key,
                                   PRIVATE_CLIENT, 0))
            return FALSE;
        xwl_screencast_client_private_registered = TRUE;
    }

    if (!xwl_screencast_client_callback_registered) {
        if (!AddCallback(&ClientStateCallback,
                         xwl_screencast_client_state_callback, NULL))
            return FALSE;
        xwl_screencast_client_callback_registered = TRUE;
    }

    screencast = calloc(1, sizeof *screencast);
    if (!screencast)
        return FALSE;

    screencast->xwl_screen = xwl_screen;
    screencast->notify_fds[0] = -1;
    screencast->notify_fds[1] = -1;
    pthread_mutex_init(&screencast->mutex, NULL);
    pthread_cond_init(&screencast->cond, NULL);

    if (pipe(screencast->notify_fds) < 0 ||
        !set_fd_flags(screencast->notify_fds[0]) ||
        !set_fd_flags(screencast->notify_fds[1])) {
        if (screencast->notify_fds[0] >= 0)
            close(screencast->notify_fds[0]);
        if (screencast->notify_fds[1] >= 0)
            close(screencast->notify_fds[1]);
        pthread_cond_destroy(&screencast->cond);
        pthread_mutex_destroy(&screencast->mutex);
        free(screencast);
        return FALSE;
    }

    SetNotifyFd(screencast->notify_fds[0], xwl_screencast_notify,
                X_NOTIFY_READ, screencast);

    xwl_screen->screencast = screencast;

    return TRUE;
}

void
xwl_screencast_fini(struct xwl_screen *xwl_screen)
{
    struct xwl_screencast *screencast = xwl_screen->screencast;

    if (!screencast)
        return;

    pthread_mutex_lock(&screencast->mutex);
    screencast->stop = TRUE;
    {
        struct xwl_screencast_pending *pending;

        for (pending = screencast->pending; pending; pending = pending->next) {
            if (pending->client) {
                struct xwl_screencast_client *sc_client =
                    dixLookupPrivate(&pending->client->devPrivates,
                                     &xwl_screencast_client_private_key);

                if (sc_client && sc_client->pending == pending)
                    sc_client->pending = NULL;
            }
            pending->client = NULL;
            pending->cancelled = TRUE;
            if (pending->fd >= 0)
                shutdown(pending->fd, SHUT_RDWR);
        }
    }
    pthread_cond_broadcast(&screencast->cond);
    pthread_mutex_unlock(&screencast->mutex);

    if (screencast->started)
        pthread_join(screencast->thread, NULL);

    for (;;) {
        struct xwl_screencast_pending *pending;

        pthread_mutex_lock(&screencast->mutex);
        pending = screencast->pending;
        pthread_mutex_unlock(&screencast->mutex);
        if (!pending)
            break;
        xwl_screencast_pending_destroy(pending);
    }

    /* The one-shot screenshot helper stores frames without a PipeWire thread. */
    pipewire_cleanup(screencast);

    RemoveNotifyFd(screencast->notify_fds[0]);
    close(screencast->notify_fds[0]);
    close(screencast->notify_fds[1]);

    free(screencast->bus_address);
    pthread_cond_destroy(&screencast->cond);
    pthread_mutex_destroy(&screencast->mutex);
    free(screencast);
    xwl_screen->screencast = NULL;
}

Bool
xwl_screencast_get_image(struct xwl_screen *xwl_screen,
                         ClientPtr client,
                         DrawablePtr drawable,
                         int x, int y, int width, int height,
                         unsigned int format,
                         unsigned long plane_mask,
                         char *dst)
{
    struct xwl_screencast *screencast = xwl_screen->screencast;
    int bits_per_pixel, dst_stride;
    int root_x, root_y;
    int stream_offset_x = 0;
    int stream_offset_y = 0;
    uint32_t i;
    Bool have_valid_stream = FALSE;
    Bool copied = FALSE;
    int64_t start_ms;
    enum xwl_screencast_frame_status status;

    if (!screencast || !xwl_screen->enable_screencast_portal)
        return FALSE;

    if (!xwl_screencast_root_position_for_drawable(xwl_screen, drawable,
                                                   x, y,
                                                   &root_x, &root_y))
        return FALSE;

    if (format != ZPixmap)
        return FALSE;

    start_ms = xwl_screencast_monotonic_msec();
    xwl_screencast_log("Xwayland screenshot portal: GetImage request "
                       "client=%d mask=0x%lx drawable=%u depth=%u "
                       "request=%dx%d+%d+%d root=%dx%d",
                       client ? client->index : -1,
                       client ? (unsigned long) client->clientAsMask : 0,
                       drawable->id, drawable->depth, width, height,
                       root_x, root_y, drawable->pScreen->width,
                       drawable->pScreen->height);

    bits_per_pixel = BitsPerPixel(drawable->depth);
    dst_stride = PixmapBytePad(width, drawable->depth);

    status = xwl_screencast_ensure_frame(screencast, client);
    if (status != XWL_SCREENCAST_FRAME_OK) {
        if (!screencast->logged_no_frame) {
            xwl_screencast_log("Xwayland screenshot portal: no portal "
                               "screenshot frame available for GetImage, "
                               "returning a black image");
            screencast->logged_no_frame = TRUE;
        }
        memset(dst, 0, (size_t) dst_stride * height);
        xwl_screencast_log("Xwayland screenshot portal: GetImage request "
                           "client=%d status=%s returned black image "
                           "elapsed=%" PRId64 "ms",
                           client ? client->index : -1,
                           xwl_screencast_frame_status_name(status),
                           xwl_screencast_monotonic_msec() - start_ms);
        return TRUE;
    }

    memset(dst, 0, (size_t) dst_stride * height);

    pthread_mutex_lock(&screencast->mutex);
    for (i = 0; i < screencast->n_streams; i++) {
        struct xwl_screencast_stream *stream = &screencast->streams[i];

        if (!stream->frame_valid || !stream->frame)
            continue;

        if (!have_valid_stream) {
            stream_offset_x = stream->x;
            stream_offset_y = stream->y;
            have_valid_stream = TRUE;
        } else {
            stream_offset_x = min(stream_offset_x, stream->x);
            stream_offset_y = min(stream_offset_y, stream->y);
        }
    }

    for (i = 0; i < screencast->n_streams; i++) {
        copied |= copy_stream_to_ximage(&screencast->streams[i],
                                        drawable->pScreen->width,
                                        drawable->pScreen->height,
                                        stream_offset_x, stream_offset_y,
                                        root_x, root_y, width, height,
                                        dst_stride, bits_per_pixel,
                                        plane_mask, (uint8_t *) dst);
    }

    if (!copied && have_valid_stream) {
        if (!screencast->logged_no_overlap) {
            xwl_screencast_log("Xwayland screencast portal: portal stream "
                               "geometry did not overlap X root request, "
                               "scaling first stream to requested image");
            screencast->logged_no_overlap = TRUE;
        }
        for (i = 0; i < screencast->n_streams; i++) {
            if (copy_stream_scaled_to_ximage(&screencast->streams[i],
                                             width, height,
                                             dst_stride, bits_per_pixel,
                                             plane_mask, (uint8_t *) dst)) {
                copied = TRUE;
                break;
            }
        }
    }
    pthread_mutex_unlock(&screencast->mutex);

    xwl_screencast_log("Xwayland screenshot portal: GetImage request "
                       "client=%d copied=%d elapsed=%" PRId64 "ms",
                       client ? client->index : -1, copied,
                       xwl_screencast_monotonic_msec() - start_ms);

    return TRUE;
}

Bool
xwl_screencast_copy_area(struct xwl_screen *xwl_screen,
                         ClientPtr client,
                         DrawablePtr src, DrawablePtr dst,
                         GCPtr gc,
                         int src_x, int src_y,
                         int width, int height,
                         int dst_x, int dst_y,
                         RegionPtr *exposed)
{
    struct xwl_screencast *screencast = xwl_screen->screencast;
    int root_x, root_y;
    int stride;
    char *buf;
    int64_t start_ms;

    if (!screencast || !xwl_screen->enable_screencast_portal)
        return FALSE;

    if (!xwl_screencast_root_position_for_drawable(xwl_screen, src,
                                                   src_x, src_y,
                                                   &root_x, &root_y))
        return FALSE;

    if (src->depth != dst->depth)
        return FALSE;

    start_ms = xwl_screencast_monotonic_msec();
    xwl_screencast_log("Xwayland screenshot portal: CopyArea request "
                       "client=%d mask=0x%lx src=%u dst=%u depth=%u "
                       "request=%dx%d+%d+%d",
                       client ? client->index : -1,
                       client ? (unsigned long) client->clientAsMask : 0,
                       src->id, dst->id, src->depth, width, height,
                       root_x, root_y);

    stride = PixmapBytePad(width, src->depth);
    buf = malloc((size_t) stride * height);
    if (!buf) {
        xwl_screencast_log("Xwayland screenshot portal: CopyArea request "
                           "client=%d failed to allocate buffer "
                           "elapsed=%" PRId64 "ms",
                           client ? client->index : -1,
                           xwl_screencast_monotonic_msec() - start_ms);
        return FALSE;
    }

    if (!xwl_screencast_get_image(xwl_screen, client, src,
                                  src_x, src_y, width, height,
                                  ZPixmap, gc->planemask, buf)) {
        xwl_screencast_log("Xwayland screenshot portal: CopyArea request "
                           "client=%d failed to obtain source image "
                           "elapsed=%" PRId64 "ms",
                           client ? client->index : -1,
                           xwl_screencast_monotonic_msec() - start_ms);
        free(buf);
        return FALSE;
    }

    (*gc->ops->PutImage) (dst, gc, dst->depth, dst_x, dst_y,
                          width, height, 0, ZPixmap, buf);
    free(buf);

    if (exposed)
        *exposed = NULL;

    xwl_screencast_log("Xwayland screenshot portal: CopyArea request "
                       "client=%d completed elapsed=%" PRId64 "ms",
                       client ? client->index : -1,
                       xwl_screencast_monotonic_msec() - start_ms);

    return TRUE;
}

Bool
xwl_screencast_name_window_pixmap(struct xwl_screen *xwl_screen,
                                  ClientPtr client,
                                  WindowPtr window,
                                  PixmapPtr pixmap,
                                  CARD32 pixmap_id)
{
    struct xwl_screencast *screencast = xwl_screen->screencast;
    ScreenPtr screen;
    GCPtr gc;
    int width, height, stride;
    char *buf;

    (void) pixmap_id;

    if (!screencast || !xwl_screen->enable_screencast_portal)
        return FALSE;

    if (!window || !pixmap)
        return FALSE;

    if (!xwl_screencast_is_root_image_drawable(xwl_screen,
                                               &window->drawable))
        return FALSE;

    screen = window->drawable.pScreen;
    width = pixmap->drawable.width;
    height = pixmap->drawable.height;
    if (width <= 0 || height <= 0)
        return FALSE;
    if (pixmap->drawable.depth != window->drawable.depth)
        return FALSE;

    stride = PixmapBytePad(width, pixmap->drawable.depth);
    buf = malloc((size_t) stride * height);
    if (!buf)
        return FALSE;

    if (!xwl_screencast_get_image(xwl_screen, client, &window->drawable,
                                  0, 0, width, height,
                                  ZPixmap, ~0UL, buf)) {
        free(buf);
        return FALSE;
    }

    gc = GetScratchGC(pixmap->drawable.depth, screen);
    if (!gc) {
        free(buf);
        return FALSE;
    }

    ValidateGC(&pixmap->drawable, gc);
    (*gc->ops->PutImage) (&pixmap->drawable, gc, pixmap->drawable.depth,
                          0, 0, width, height, 0, ZPixmap, buf);
    FreeScratchGC(gc);
    free(buf);

    return TRUE;
}
