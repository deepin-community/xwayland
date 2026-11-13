/*
 * Copyright (C) 2026
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation.
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include <dbus/dbus.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib.h>
#include <pipewire/pipewire.h>
#include <spa/buffer/buffer.h>
#include <spa/param/buffers.h>
#include <spa/param/format-utils.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/raw-utils.h>
#include <spa/pod/vararg.h>
#include <spa/utils/result.h>

#include "xwayland-screencast-protocol.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#define PORTAL_BUS_NAME "org.freedesktop.portal.Desktop"
#define PORTAL_OBJECT_PATH "/org/freedesktop/portal/desktop"
#define PORTAL_SCREENCAST_IFACE "org.freedesktop.portal.ScreenCast"
#define PORTAL_SCREENSHOT_IFACE "org.freedesktop.portal.Screenshot"
#define PORTAL_REQUEST_IFACE "org.freedesktop.portal.Request"
#define PORTAL_PROPERTIES_IFACE "org.freedesktop.DBus.Properties"
#define PORTAL_HOST_REGISTRY_IFACE "org.freedesktop.host.portal.Registry"

#define PORTAL_SCREENCAST_SOURCE_MONITOR 1
#define PORTAL_SCREENCAST_CURSOR_HIDDEN 1
#define PORTAL_SCREENCAST_CURSOR_EMBEDDED 2
#define PORTAL_SCREENCAST_PERSIST_MODE_PERSISTENT 2

#define HELPER_START_TIMEOUT_MS 30000
#define HELPER_DBUS_CALL_TIMEOUT_MS 5000
#define HELPER_DBUS_CLOSE_TIMEOUT_MS 1000
#define HELPER_ACCEPT_POLL_TIMEOUT_MS 250
#define SD_LISTEN_FDS_START 3
#define XWL_SCREENCAST_BPP 4
#define XWL_SCREENCAST_BUFFER_TYPES (1u << SPA_DATA_MemFd)

struct helper;

struct helper_stream {
    struct helper *helper;

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
    bool frame_valid;
    bool logged_unsupported_buffer;
    bool logged_short_buffer;
};

struct helper {
    char *app_id;
    char *app_name;
    char *restore_token;
    struct helper *next;

    pthread_mutex_t mutex;
    pthread_cond_t cond;
    pthread_t thread;

    bool started;
    bool thread_finished;
    bool failed;
    bool stop;
    bool have_frame;
    const char *stage;

    DBusConnection *connection;
    char *session_handle;
    char *response_match;

    struct pw_thread_loop *pw_loop;
    struct pw_context *pw_context;
    struct pw_core *pw_core;

    struct helper_stream *streams;
    uint32_t n_streams;
};

static pthread_mutex_t helpers_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct helper *helpers;
static volatile sig_atomic_t stop_requested;
static char listen_path[PATH_MAX];

static void timespec_add_ms(struct timespec *ts, int ms);
static void portal_cleanup(struct helper *helper);

static void __attribute__((format(printf, 1, 2)))
helper_log(const char *format, ...)
{
    char message[1024];
    va_list args;

    va_start(args, format);
    vsnprintf(message, sizeof message, format, args);
    va_end(args);

    fprintf(stderr, "%s\n", message);
    openlog("xwayland-screencast-helper", LOG_PID, LOG_USER);
    syslog(LOG_INFO, "%s", message);
}

static void
helper_make_token(char *buf, size_t size, const char *prefix)
{
    static uint32_t serial;

    snprintf(buf, size, "%s_%u_%u", prefix, (unsigned) getpid(), ++serial);
}

static bool
read_full(int fd, void *data, size_t size)
{
    uint8_t *ptr = data;

    while (size > 0) {
        ssize_t len = read(fd, ptr, size);

        if (len < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (len == 0)
            return false;

        ptr += len;
        size -= len;
    }

    return true;
}

static bool
write_full(int fd, const void *data, size_t size)
{
    const uint8_t *ptr = data;

    while (size > 0) {
        ssize_t len = write(fd, ptr, size);

        if (len < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (len == 0)
            return false;

        ptr += len;
        size -= len;
    }

    return true;
}

static bool
pread_full(int fd, void *data, size_t size, off_t offset)
{
    uint8_t *ptr = data;

    while (size > 0) {
        ssize_t len = pread(fd, ptr, size, offset);

        if (len < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (len == 0)
            return false;

        ptr += len;
        size -= len;
        offset += len;
    }

    return true;
}

static void
set_close_on_exec(int fd)
{
    int flags = fcntl(fd, F_GETFD);

    if (flags >= 0)
        fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

static bool
set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL);

    if (flags < 0)
        return false;

    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static bool
helper_app_id_is_valid(const char *app_id)
{
    bool seen_dot = false;
    size_t i;

    if (!app_id || app_id[0] == '\0' || app_id[0] == '.' ||
        strlen(app_id) >= XWL_SCREENCAST_HELPER_APP_ID_SIZE)
        return false;

    for (i = 0; app_id[i] != '\0'; i++) {
        char c = app_id[i];

        if (c == '.') {
            if (i == 0 || app_id[i + 1] == '\0')
                return false;
            seen_dot = true;
            continue;
        }

        if (!((c >= 'A' && c <= 'Z') ||
              (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') ||
              c == '_' || c == '-'))
            return false;
    }

    return seen_dot;
}

static const char *
helper_safe_app_id(const char *app_id)
{
    return helper_app_id_is_valid(app_id) ? app_id : "xwayland.unknown";
}

static const char *
helper_safe_app_name(const char *app_name, const char *app_id)
{
    return app_name && app_name[0] != '\0' ? app_name : app_id;
}

static bool
mkdir_p(const char *path, mode_t mode)
{
    char tmp[PATH_MAX];
    size_t len;
    char *p;

    if (!path || path[0] == '\0')
        return false;

    snprintf(tmp, sizeof tmp, "%s", path);
    len = strlen(tmp);
    if (len == 0 || len >= sizeof tmp)
        return false;
    if (tmp[len - 1] == '/')
        tmp[len - 1] = '\0';

    for (p = tmp + 1; *p; p++) {
        if (*p != '/')
            continue;

        *p = '\0';
        if (mkdir(tmp, mode) < 0 && errno != EEXIST)
            return false;
        *p = '/';
    }

    if (mkdir(tmp, mode) < 0 && errno != EEXIST)
        return false;

    return true;
}

static void
desktop_value_sanitize(char *dst, size_t dst_size, const char *src)
{
    size_t i, j = 0;

    if (!src || src[0] == '\0')
        src = "Xwayland Application";

    for (i = 0; src[i] != '\0' && j + 1 < dst_size; i++) {
        unsigned char c = src[i];

        if (c < 0x20 || c == 0x7f)
            c = ' ';
        dst[j++] = c;
    }

    while (j > 0 && dst[j - 1] == ' ')
        j--;
    dst[j] = '\0';

    if (dst[0] == '\0')
        snprintf(dst, dst_size, "%s", "Xwayland Application");
}

static bool
helper_ensure_desktop_file(const char *app_id, const char *app_name)
{
    const char *xdg_data_home = getenv("XDG_DATA_HOME");
    const char *home = getenv("HOME");
    char applications_dir[PATH_MAX];
    char desktop_path[PATH_MAX];
    char name[256];
    char content[1024];
    int fd;
    int len;
    int path_len;
    bool ok;

    if (xdg_data_home && xdg_data_home[0] == '/') {
        path_len = snprintf(applications_dir, sizeof applications_dir,
                            "%s/applications", xdg_data_home);
    } else if (home && home[0] == '/') {
        path_len = snprintf(applications_dir, sizeof applications_dir,
                            "%s/.local/share/applications", home);
    } else {
        return false;
    }

    if (path_len < 0 || (size_t) path_len >= sizeof applications_dir)
        return false;

    if (!mkdir_p(applications_dir, 0700))
        return false;

    path_len = snprintf(desktop_path, sizeof desktop_path, "%s/%s.desktop",
                        applications_dir, app_id);
    if (path_len < 0 || (size_t) path_len >= sizeof desktop_path)
        return false;

    if (access(desktop_path, F_OK) == 0)
        return true;

    desktop_value_sanitize(name, sizeof name, app_name);
    len = snprintf(content, sizeof content,
                   "[Desktop Entry]\n"
                   "Type=Application\n"
                   "Name=%s\n"
                   "Exec=true\n"
                   "NoDisplay=true\n"
                   "X-Xwayland-Screencast-Helper=true\n",
                   name);
    if (len < 0 || (size_t) len >= sizeof content)
        return false;

    fd = open(desktop_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0)
        return errno == EEXIST;

    ok = write_full(fd, content, (size_t) len);
    close(fd);
    if (!ok)
        unlink(desktop_path);

    return ok;
}

static bool
helper_restore_token_is_valid(const char *token)
{
    size_t i;

    if (!token || strlen(token) != 36)
        return false;

    for (i = 0; i < 36; i++) {
        char c = token[i];

        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-')
                return false;
            continue;
        }

        if (!((c >= '0' && c <= '9') ||
              (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F')))
            return false;
    }

    return true;
}

static bool
helper_restore_token_dir(char *dir, size_t size)
{
    const char *xdg_state_home = getenv("XDG_STATE_HOME");
    const char *home = getenv("HOME");
    int len;

    if (xdg_state_home && xdg_state_home[0] == '/') {
        len = snprintf(dir, size, "%s/xwayland-screencast-helper",
                       xdg_state_home);
    } else if (home && home[0] == '/') {
        len = snprintf(dir, size, "%s/.local/state/xwayland-screencast-helper",
                       home);
    } else {
        return false;
    }

    if (len < 0 || (size_t) len >= size)
        return false;

    return mkdir_p(dir, 0700);
}

static bool
helper_restore_token_path(const char *app_id, char *path, size_t size)
{
    char dir[PATH_MAX];
    int len;

    if (!helper_restore_token_dir(dir, sizeof dir))
        return false;

    len = snprintf(path, size, "%s/%s.restore-token", dir,
                   helper_safe_app_id(app_id));
    return len >= 0 && (size_t) len < size;
}

static void
helper_delete_restore_token(const char *app_id)
{
    char path[PATH_MAX];

    if (helper_restore_token_path(app_id, path, sizeof path))
        unlink(path);
}

static char *
helper_load_restore_token(const char *app_id)
{
    char path[PATH_MAX];
    char buf[128];
    ssize_t len;
    int fd;

    if (!helper_restore_token_path(app_id, path, sizeof path))
        return NULL;

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return NULL;

    len = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (len <= 0)
        return NULL;

    while (len > 0 &&
           (buf[len - 1] == '\n' ||
            buf[len - 1] == '\r' ||
            buf[len - 1] == ' ' ||
            buf[len - 1] == '\t'))
        len--;
    buf[len] = '\0';

    if (!helper_restore_token_is_valid(buf)) {
        unlink(path);
        return NULL;
    }

    return strdup(buf);
}

static bool
helper_save_restore_token(const char *app_id, const char *token)
{
    char path[PATH_MAX];
    char content[64];
    int fd;
    int len;
    bool ok;

    if (!helper_restore_token_is_valid(token))
        return false;

    if (!helper_restore_token_path(app_id, path, sizeof path))
        return false;

    len = snprintf(content, sizeof content, "%s\n", token);
    if (len < 0 || (size_t) len >= sizeof content)
        return false;

    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0)
        return false;

    ok = write_full(fd, content, (size_t) len);
    close(fd);
    if (!ok)
        unlink(path);

    return ok;
}

static struct helper *
helper_new(const char *app_id, const char *app_name)
{
    struct helper *helper = calloc(1, sizeof *helper);

    if (!helper)
        return NULL;

    helper->app_id = strdup(app_id);
    helper->app_name = strdup(helper_safe_app_name(app_name, app_id));
    if (!helper->app_id || !helper->app_name) {
        free(helper->app_id);
        free(helper->app_name);
        free(helper);
        return NULL;
    }
    helper->restore_token = helper_load_restore_token(helper->app_id);

    pthread_mutex_init(&helper->mutex, NULL);
    pthread_cond_init(&helper->cond, NULL);
    return helper;
}

static void
helper_free_one(struct helper *helper)
{
    if (!helper)
        return;

    pthread_cond_destroy(&helper->cond);
    pthread_mutex_destroy(&helper->mutex);
    free(helper->app_id);
    free(helper->app_name);
    free(helper->restore_token);
    free(helper);
}

static struct helper *
helper_for_app_id(const char *app_id, const char *app_name)
{
    struct helper *helper;

    app_id = helper_safe_app_id(app_id);
    app_name = helper_safe_app_name(app_name, app_id);

    pthread_mutex_lock(&helpers_mutex);
    for (helper = helpers; helper; helper = helper->next) {
        if (strcmp(helper->app_id, app_id) == 0) {
            pthread_mutex_unlock(&helpers_mutex);
            return helper;
        }
    }

    helper = helper_new(app_id, app_name);
    if (helper) {
        helper->next = helpers;
        helpers = helper;
    }
    pthread_mutex_unlock(&helpers_mutex);

    return helper;
}

static bool
append_dict_entry_uint32(DBusMessageIter *dict, const char *key, uint32_t value)
{
    DBusMessageIter entry, variant;
    const char *key_arg = key;

    if (!dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry))
        return false;
    if (!dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key_arg))
        return false;
    if (!dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "u", &variant))
        return false;
    if (!dbus_message_iter_append_basic(&variant, DBUS_TYPE_UINT32, &value))
        return false;
    if (!dbus_message_iter_close_container(&entry, &variant))
        return false;
    if (!dbus_message_iter_close_container(dict, &entry))
        return false;

    return true;
}

static bool
append_dict_entry_bool(DBusMessageIter *dict, const char *key, dbus_bool_t value)
{
    DBusMessageIter entry, variant;
    const char *key_arg = key;

    if (!dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry))
        return false;
    if (!dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key_arg))
        return false;
    if (!dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "b", &variant))
        return false;
    if (!dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &value))
        return false;
    if (!dbus_message_iter_close_container(&entry, &variant))
        return false;
    if (!dbus_message_iter_close_container(dict, &entry))
        return false;

    return true;
}

static bool
append_dict_entry_string(DBusMessageIter *dict, const char *key,
                         const char *value)
{
    DBusMessageIter entry, variant;
    const char *key_arg = key;
    const char *value_arg = value;

    if (!dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry))
        return false;
    if (!dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key_arg))
        return false;
    if (!dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &variant))
        return false;
    if (!dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &value_arg))
        return false;
    if (!dbus_message_iter_close_container(&entry, &variant))
        return false;
    if (!dbus_message_iter_close_container(dict, &entry))
        return false;

    return true;
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
        helper_log("Xwayland screencast helper D-Bus error: %s", error.message);
        dbus_error_free(&error);
    }

    return reply;
}

static DBusMessage *
portal_call(DBusConnection *connection, DBusMessage *message)
{
    return portal_call_with_timeout(connection, message,
                                    HELPER_DBUS_CALL_TIMEOUT_MS);
}

static bool
portal_register_app_id(struct helper *helper)
{
    DBusMessage *message;
    DBusMessage *reply;
    DBusMessageIter iter, options;
    const char *app_id = helper->app_id;

    if (!helper_ensure_desktop_file(helper->app_id, helper->app_name)) {
        helper_log("Xwayland screencast helper: failed to prepare desktop "
                   "file for app id %s", helper->app_id);
    }

    message = dbus_message_new_method_call(PORTAL_BUS_NAME,
                                           PORTAL_OBJECT_PATH,
                                           PORTAL_HOST_REGISTRY_IFACE,
                                           "Register");
    if (!message)
        return false;

    dbus_message_iter_init_append(message, &iter);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &app_id);
    if (!dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}",
                                          &options))
        goto out_message;
    if (!dbus_message_iter_close_container(&iter, &options))
        goto out_message;

    reply = portal_call(helper->connection, message);
    message = NULL;
    if (!reply) {
        helper_log("Xwayland screencast helper: failed to register app id "
                   "%s with portal",
                   helper->app_id);
        return false;
    }

    if (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR) {
        const char *error_name = dbus_message_get_error_name(reply);

        helper_log("Xwayland screencast helper: portal rejected app id %s: %s",
                   helper->app_id,
                   error_name ? error_name : "unknown D-Bus error");
        dbus_message_unref(reply);
        return false;
    }

    dbus_message_unref(reply);
    return true;

out_message:
    if (message)
        dbus_message_unref(message);
    return false;
}

static void
helper_set_failed(struct helper *helper)
{
    pthread_mutex_lock(&helper->mutex);
    helper->failed = true;
    pthread_cond_broadcast(&helper->cond);
    pthread_mutex_unlock(&helper->mutex);
}

static void
helper_set_stage(struct helper *helper, const char *stage)
{
    pthread_mutex_lock(&helper->mutex);
    helper->stage = stage;
    pthread_cond_broadcast(&helper->cond);
    pthread_mutex_unlock(&helper->mutex);
}

static bool
helper_stopped(struct helper *helper)
{
    bool stopped;

    pthread_mutex_lock(&helper->mutex);
    stopped = helper->stop;
    pthread_mutex_unlock(&helper->mutex);

    return stopped;
}

static int64_t
helper_monotonic_msec(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static uint32_t
helper_next_request_id(void)
{
    static uint32_t serial;

    return ++serial;
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
    if (len < 0)
        return NULL;

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
        helper_log("Xwayland screencast helper D-Bus match error: %s",
                   error.message);
        dbus_error_free(&error);
        free(rule);
        return NULL;
    }

    return rule;
}

static DBusMessage *
portal_wait_response_timed(struct helper *helper, const char *path,
                           int timeout_ms)
{
    DBusConnection *connection = helper->connection;
    DBusMessage *message;
    int64_t deadline = timeout_ms >= 0 ?
        helper_monotonic_msec() + timeout_ms : 0;

    while (!helper_stopped(helper)) {
        int timeout = 250;

        if (timeout_ms >= 0) {
            int64_t remaining = deadline - helper_monotonic_msec();

            if (remaining <= 0)
                break;
            if (remaining < timeout)
                timeout = remaining;
        }

        dbus_connection_read_write(connection, timeout);

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

static DBusMessage *
portal_wait_response(struct helper *helper, const char *path)
{
    return portal_wait_response_timed(helper, path, -1);
}

static bool
helper_client_disconnected(int fd, short revents)
{
    char byte;
    ssize_t len;

    if (revents & (POLLERR | POLLHUP | POLLNVAL))
        return true;
#ifdef POLLRDHUP
    if (revents & POLLRDHUP)
        return true;
#endif
    if (!(revents & POLLIN))
        return false;

    len = recv(fd, &byte, sizeof byte, MSG_PEEK | MSG_DONTWAIT);
    if (len == 0)
        return true;
    if (len < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
        return true;

    return false;
}

static DBusMessage *
portal_wait_response_cancellable(struct helper *helper,
                                 const char *path,
                                 int client_fd,
                                 bool *cancelled_out)
{
    DBusConnection *connection = helper->connection;
    DBusMessage *message;
    int bus_fd = -1;

    *cancelled_out = false;
    dbus_connection_get_unix_fd(connection, &bus_fd);

    while (!helper_stopped(helper)) {
        while ((message = dbus_connection_pop_message(connection)) != NULL) {
            const char *message_path = dbus_message_get_path(message);

            if (dbus_message_is_signal(message, PORTAL_REQUEST_IFACE, "Response") &&
                message_path && strcmp(message_path, path) == 0)
                return message;

            dbus_message_unref(message);
        }

        if (bus_fd < 0) {
            if (helper_client_disconnected(client_fd, POLLIN)) {
                *cancelled_out = true;
                return NULL;
            }
            if (!dbus_connection_read_write(connection, 250))
                return NULL;
            continue;
        }

        {
            struct pollfd fds[2] = {
                {
                    .fd = bus_fd,
                    .events = POLLIN,
                },
                {
                    .fd = client_fd,
                    .events = POLLIN
#ifdef POLLRDHUP
                              | POLLRDHUP
#endif
                    ,
                },
            };
            int ready = poll(fds, 2, 250);

            if (ready < 0) {
                if (errno == EINTR)
                    continue;
                return NULL;
            }
            if (ready == 0)
                continue;

            if (helper_client_disconnected(client_fd, fds[1].revents)) {
                *cancelled_out = true;
                return NULL;
            }

            if (fds[0].revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL)) {
                if (!dbus_connection_read_write(connection, 0))
                    return NULL;
            }
        }
    }

    return NULL;
}

static void
portal_close_request(DBusConnection *connection, const char *path)
{
    DBusMessage *message;
    dbus_uint32_t serial = 0;

    if (!connection || !path)
        return;

    message = dbus_message_new_method_call(PORTAL_BUS_NAME,
                                           path,
                                           PORTAL_REQUEST_IFACE,
                                           "Close");
    if (!message)
        return;

    if (!dbus_connection_send(connection, message, &serial))
        helper_log("Xwayland screenshot portal: failed to send Close for "
                   "request %s", path);
    dbus_connection_flush(connection);
    dbus_message_unref(message);
}

static bool
portal_response_success(DBusMessage *message,
                        DBusMessageIter *results,
                        uint32_t *response_out)
{
    DBusMessageIter iter;
    uint32_t response;

    if (response_out)
        *response_out = UINT32_MAX;

    if (!dbus_message_iter_init(message, &iter))
        return false;

    if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_UINT32)
        return false;

    dbus_message_iter_get_basic(&iter, &response);
    if (response_out)
        *response_out = response;
    if (response != 0) {
        helper_log("Xwayland screencast helper: request returned response %u",
                   response);
        return false;
    }

    if (!dbus_message_iter_next(&iter))
        return false;
    if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY)
        return false;

    *results = iter;
    return true;
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
        if (value_type == DBUS_TYPE_STRING ||
            value_type == DBUS_TYPE_OBJECT_PATH) {
            const char *value;

            dbus_message_iter_get_basic(&variant, &value);
            return strdup(value);
        }

next:
        dbus_message_iter_next(&dict);
    }

    return NULL;
}

static bool
portal_open_request_connection(struct helper *helper)
{
    DBusError error;

    helper_set_stage(helper, "connecting to session bus");
    dbus_error_init(&error);
    helper->connection = dbus_bus_get_private(DBUS_BUS_SESSION, &error);
    if (!helper->connection) {
        if (dbus_error_is_set(&error)) {
            helper_log("Xwayland screenshot portal D-Bus session bus error: %s",
                       error.message);
            dbus_error_free(&error);
        }
        return false;
    }

    dbus_connection_set_exit_on_disconnect(helper->connection, false);
    if (!portal_register_app_id(helper))
        return false;

    helper_set_stage(helper, "subscribing to portal responses");
    helper->response_match = portal_add_response_match(helper->connection, NULL);
    return helper->response_match != NULL;
}

static bool
portal_screenshot_decode_uri(const char *uri,
                             struct xwl_screencast_helper_response *resp,
                             uint8_t **frame_out)
{
    GError *error = NULL;
    GdkPixbuf *pixbuf;
    gchar *path;
    uint8_t *frame;
    const guchar *pixels;
    int width, height, src_stride, n_channels;
    int dst_stride;
    size_t frame_size;
    gboolean has_alpha;
    int y;

    *frame_out = NULL;

    path = g_filename_from_uri(uri, NULL, &error);
    if (!path) {
        if (error) {
            helper_log("Xwayland screenshot portal: failed to convert URI "
                       "%s: %s", uri, error->message);
            g_clear_error(&error);
        }
        if (uri[0] == '/')
            path = g_strdup(uri);
    }
    if (!path)
        return false;

    pixbuf = gdk_pixbuf_new_from_file(path, &error);
    if (!pixbuf) {
        if (error) {
            helper_log("Xwayland screenshot portal: failed to decode %s: %s",
                       path, error->message);
            g_clear_error(&error);
        }
        g_free(path);
        return false;
    }

    width = gdk_pixbuf_get_width(pixbuf);
    height = gdk_pixbuf_get_height(pixbuf);
    src_stride = gdk_pixbuf_get_rowstride(pixbuf);
    n_channels = gdk_pixbuf_get_n_channels(pixbuf);
    has_alpha = gdk_pixbuf_get_has_alpha(pixbuf);
    pixels = gdk_pixbuf_get_pixels(pixbuf);

    if (width <= 0 || height <= 0 || src_stride <= 0 ||
        n_channels < 3 || n_channels > 4 ||
        gdk_pixbuf_get_bits_per_sample(pixbuf) != 8 ||
        width > INT_MAX / XWL_SCREENCAST_BPP ||
        (size_t) height > SIZE_MAX / ((size_t) width * XWL_SCREENCAST_BPP)) {
        helper_log("Xwayland screenshot portal: unsupported screenshot "
                   "image geometry/channels %dx%d channels=%d stride=%d",
                   width, height, n_channels, src_stride);
        g_object_unref(pixbuf);
        g_free(path);
        return false;
    }

    dst_stride = width * XWL_SCREENCAST_BPP;
    frame_size = (size_t) dst_stride * height;
    frame = malloc(frame_size);
    if (!frame) {
        g_object_unref(pixbuf);
        g_free(path);
        return false;
    }

    for (y = 0; y < height; y++) {
        const guchar *src = pixels + (size_t) y * src_stride;
        uint8_t *dst = frame + (size_t) y * dst_stride;
        int x;

        for (x = 0; x < width; x++) {
            const guchar *p = src + (size_t) x * n_channels;
            uint8_t *q = dst + (size_t) x * XWL_SCREENCAST_BPP;

            q[0] = p[2];
            q[1] = p[1];
            q[2] = p[0];
            q[3] = has_alpha && n_channels == 4 ? p[3] : 0xff;
        }
    }

    memset(resp, 0, sizeof *resp);
    resp->magic = XWL_SCREENCAST_HELPER_MAGIC;
    resp->version = XWL_SCREENCAST_HELPER_VERSION;
    resp->status = XWL_SCREENCAST_HELPER_STATUS_OK;
    resp->format = has_alpha ? SPA_VIDEO_FORMAT_BGRA : SPA_VIDEO_FORMAT_BGRx;
    resp->stream_x = 0;
    resp->stream_y = 0;
    resp->stream_width = width;
    resp->stream_height = height;
    resp->frame_width = width;
    resp->frame_height = height;
    resp->frame_stride = dst_stride;
    resp->frame_size = frame_size;
    *frame_out = frame;

    if (unlink(path) < 0 && errno != ENOENT)
        helper_log("Xwayland screenshot portal: failed to remove temporary "
                   "screenshot %s: %s", path, strerror(errno));

    g_object_unref(pixbuf);
    g_free(path);
    return true;
}

static bool
portal_screenshot_get_frame(struct helper *helper,
                            uint32_t request_id,
                            int client_fd,
                            struct xwl_screencast_helper_response *resp,
                            uint8_t **frame_out)
{
    DBusMessage *message, *reply, *response;
    DBusMessageIter iter, options, results;
    char handle_token[64];
    const char *parent_window = "";
    const char *request_path_arg = NULL;
    char *request_path = NULL;
    char *uri = NULL;
    dbus_bool_t modal = false;
    dbus_bool_t interactive = false;
    bool ok = false;
    bool cancelled = false;
    uint32_t portal_response = UINT32_MAX;
    int64_t start_ms = helper_monotonic_msec();
    int64_t wait_start_ms;

    memset(resp, 0, sizeof *resp);
    resp->magic = XWL_SCREENCAST_HELPER_MAGIC;
    resp->version = XWL_SCREENCAST_HELPER_VERSION;
    resp->status = XWL_SCREENCAST_HELPER_STATUS_NO_FRAME;
    *frame_out = NULL;

    helper_log("Xwayland screenshot portal helper: request id=%u starting "
               "Screenshot portal app_id=%s app_name=%s",
               request_id, helper->app_id, helper->app_name);

    if (!portal_open_request_connection(helper))
        goto out;
    helper_log("Xwayland screenshot portal helper: request id=%u connected "
               "to session bus", request_id);

    helper_make_token(handle_token, sizeof handle_token, "xwl_ss");
    helper_set_stage(helper, "calling screenshot portal");

    message = dbus_message_new_method_call(PORTAL_BUS_NAME,
                                           PORTAL_OBJECT_PATH,
                                           PORTAL_SCREENSHOT_IFACE,
                                           "Screenshot");
    if (!message)
        goto out;

    dbus_message_iter_init_append(message, &iter);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &parent_window);
    if (!dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}",
                                          &options))
        goto out_message;
    if (!append_dict_entry_string(&options, "handle_token", handle_token))
        goto out_message;
    if (!append_dict_entry_bool(&options, "modal", modal))
        goto out_message;
    if (!append_dict_entry_bool(&options, "interactive", interactive))
        goto out_message;
    if (!dbus_message_iter_close_container(&iter, &options))
        goto out_message;

    helper_log("Xwayland screenshot portal helper: request id=%u calling "
               "org.freedesktop.portal.Screenshot.Screenshot "
               "interactive=%d modal=%d token=%s",
               request_id, interactive, modal, handle_token);
    reply = portal_call(helper->connection, message);
    message = NULL;
    if (!reply) {
        helper_log("Xwayland screenshot portal helper: request id=%u "
                   "Screenshot method call failed after %" PRId64 "ms",
                   request_id, helper_monotonic_msec() - start_ms);
        goto out;
    }

    dbus_message_get_args(reply, NULL,
                          DBUS_TYPE_OBJECT_PATH, &request_path_arg,
                          DBUS_TYPE_INVALID);
    if (!request_path_arg) {
        helper_log("Xwayland screenshot portal helper: request id=%u "
                   "Screenshot method reply did not include a request path",
                   request_id);
        dbus_message_unref(reply);
        goto out;
    }
    request_path = strdup(request_path_arg);
    if (!request_path) {
        dbus_message_unref(reply);
        goto out;
    }

    helper_set_stage(helper, "waiting for screenshot portal response");
    wait_start_ms = helper_monotonic_msec();
    helper_log("Xwayland screenshot portal helper: request id=%u waiting "
               "for Screenshot Response path=%s decision-timeout=none",
               request_id, request_path);
    response = portal_wait_response_cancellable(helper, request_path,
                                                client_fd, &cancelled);
    dbus_message_unref(reply);
    if (!response) {
        helper_log("Xwayland screenshot portal helper: request id=%u "
                   "%s while waiting for Response after %" PRId64
                   "ms stage=%s",
                   request_id,
                   cancelled ? "client disconnected" : "wait failed",
                   helper_monotonic_msec() - wait_start_ms,
                   helper->stage ? helper->stage : "unknown");
        portal_close_request(helper->connection, request_path);
        goto out;
    }

    helper_log("Xwayland screenshot portal helper: request id=%u received "
               "Screenshot Response after %" PRId64 "ms total=%" PRId64 "ms",
               request_id, helper_monotonic_msec() - wait_start_ms,
               helper_monotonic_msec() - start_ms);
    if (portal_response_success(response, &results, &portal_response)) {
        uri = response_lookup_string(&results, "uri");
    } else if (portal_response != UINT32_MAX) {
        resp->status = XWL_SCREENCAST_HELPER_STATUS_NO_FRAME;
        resp->reserved = portal_response;
        ok = true;
        helper_log("Xwayland screenshot portal helper: request id=%u "
                   "Screenshot portal returned response=%u, sending "
                   "no-frame status",
                   request_id, portal_response);
    }
    dbus_message_unref(response);

    if (ok && resp->status == XWL_SCREENCAST_HELPER_STATUS_NO_FRAME)
        goto out;

    if (!uri) {
        helper_log("Xwayland screenshot portal helper: request id=%u "
                   "screenshot response did not include an image URI",
                   request_id);
        goto out;
    }

    helper_set_stage(helper, "decoding screenshot portal image");
    helper_log("Xwayland screenshot portal helper: request id=%u decoding "
               "Screenshot image URI", request_id);
    ok = portal_screenshot_decode_uri(uri, resp, frame_out);
    if (ok) {
        helper_log("Xwayland screenshot portal helper: request id=%u decoded "
                   "Screenshot image frame=%ux%u stride=%u size=%" PRIu64
                   " total=%" PRId64 "ms",
                   request_id, resp->frame_width, resp->frame_height,
                   resp->frame_stride, resp->frame_size,
                   helper_monotonic_msec() - start_ms);
    }

out:
    if (!ok) {
        helper_log("Xwayland screenshot portal helper: request id=%u failed "
                   "stage=%s total=%" PRId64 "ms",
                   request_id, helper->stage ? helper->stage : "unknown",
                   helper_monotonic_msec() - start_ms);
    }
    free(request_path);
    free(uri);
    portal_cleanup(helper);
    return ok;

out_message:
    if (message)
        dbus_message_unref(message);
    goto out;
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
portal_create_session(struct helper *helper)
{
    DBusMessage *message, *reply, *response;
    DBusMessageIter iter, options, results;
    char handle_token[64], session_token[64];
    char *request_path = NULL;
    char *session_handle = NULL;

    helper_make_token(handle_token, sizeof handle_token, "xwl_sc_create");
    helper_make_token(session_token, sizeof session_token, "xwl_sc_session");

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

    reply = portal_call(helper->connection, message);
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

    response = portal_wait_response(helper, request_path);
    dbus_message_unref(reply);
    if (!response)
        return NULL;

    if (portal_response_success(response, &results, NULL))
        session_handle = response_lookup_string(&results, "session_handle");

    dbus_message_unref(response);
    return session_handle;

out_message:
    if (message)
        dbus_message_unref(message);
    return NULL;
}

static bool
portal_select_sources(struct helper *helper)
{
    DBusMessage *message, *reply, *response;
    DBusMessageIter iter, options, results;
    char handle_token[64];
    const char *session_handle = helper->session_handle;
    char *request_path = NULL;
    uint32_t cursor_modes;
    bool ok = false;

    helper_make_token(handle_token, sizeof handle_token, "xwl_sc_select");
    cursor_modes = portal_get_available_cursor_modes(helper->connection);

    message = dbus_message_new_method_call(PORTAL_BUS_NAME,
                                           PORTAL_OBJECT_PATH,
                                           PORTAL_SCREENCAST_IFACE,
                                           "SelectSources");
    if (!message)
        return false;

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
    if (!append_dict_entry_uint32(&options, "persist_mode",
                                  PORTAL_SCREENCAST_PERSIST_MODE_PERSISTENT))
        goto out_message;
    if (helper->restore_token &&
        helper_restore_token_is_valid(helper->restore_token) &&
        !append_dict_entry_string(&options, "restore_token",
                                  helper->restore_token))
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

    reply = portal_call(helper->connection, message);
    message = NULL;
    if (!reply)
        return false;

    dbus_message_get_args(reply, NULL,
                          DBUS_TYPE_OBJECT_PATH, &request_path,
                          DBUS_TYPE_INVALID);
    if (!request_path) {
        dbus_message_unref(reply);
        return false;
    }

    response = portal_wait_response(helper, request_path);
    dbus_message_unref(reply);
    if (!response)
        return false;

    ok = portal_response_success(response, &results, NULL);
    dbus_message_unref(response);
    return ok;

out_message:
    if (message)
        dbus_message_unref(message);
    return false;
}

static void
parse_stream_properties(DBusMessageIter *props, struct helper_stream *stream)
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

static bool
parse_streams(DBusMessageIter *results, struct helper *helper)
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
            struct helper_stream *streams;
            struct helper_stream *stream;
            uint32_t node_id;

            dbus_message_iter_recurse(&array, &tuple);
            if (dbus_message_iter_get_arg_type(&tuple) != DBUS_TYPE_UINT32)
                goto stream_next;
            dbus_message_iter_get_basic(&tuple, &node_id);

            streams = reallocarray(helper->streams,
                                   helper->n_streams + 1,
                                   sizeof *helper->streams);
            if (!streams)
                return false;

            helper->streams = streams;
            stream = &helper->streams[helper->n_streams++];
            memset(stream, 0, sizeof *stream);
            stream->helper = helper;
            stream->node_id = node_id;

            if (dbus_message_iter_next(&tuple) &&
                dbus_message_iter_get_arg_type(&tuple) == DBUS_TYPE_ARRAY)
                parse_stream_properties(&tuple, stream);

stream_next:
            dbus_message_iter_next(&array);
        }

        return helper->n_streams > 0;

next:
        dbus_message_iter_next(&dict);
    }

    return false;
}

static bool
portal_start(struct helper *helper)
{
    DBusMessage *message, *reply, *response;
    DBusMessageIter iter, options, results;
    char handle_token[64];
    const char *session_handle = helper->session_handle;
    const char *parent_window = "";
    char *request_path = NULL;
    char *restore_token = NULL;
    bool ok = false;

    helper_make_token(handle_token, sizeof handle_token, "xwl_sc_start");

    message = dbus_message_new_method_call(PORTAL_BUS_NAME,
                                           PORTAL_OBJECT_PATH,
                                           PORTAL_SCREENCAST_IFACE,
                                           "Start");
    if (!message)
        return false;

    dbus_message_iter_init_append(message, &iter);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &session_handle);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &parent_window);
    if (!dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &options))
        goto out_message;
    if (!append_dict_entry_string(&options, "handle_token", handle_token))
        goto out_message;
    if (!dbus_message_iter_close_container(&iter, &options))
        goto out_message;

    reply = portal_call(helper->connection, message);
    message = NULL;
    if (!reply)
        return false;

    dbus_message_get_args(reply, NULL,
                          DBUS_TYPE_OBJECT_PATH, &request_path,
                          DBUS_TYPE_INVALID);
    if (!request_path) {
        dbus_message_unref(reply);
        return false;
    }

    response = portal_wait_response(helper, request_path);
    dbus_message_unref(reply);
    if (!response)
        return false;

    if (portal_response_success(response, &results, NULL)) {
        ok = parse_streams(&results, helper);
        if (ok)
            restore_token = response_lookup_string(&results, "restore_token");
    }

    if (ok) {
        if (restore_token) {
            if (helper_save_restore_token(helper->app_id, restore_token)) {
                free(helper->restore_token);
                helper->restore_token = restore_token;
                restore_token = NULL;
            }
        } else if (helper->restore_token) {
            helper_delete_restore_token(helper->app_id);
            free(helper->restore_token);
            helper->restore_token = NULL;
        }
    }

    dbus_message_unref(response);
    free(restore_token);
    return ok;

out_message:
    if (message)
        dbus_message_unref(message);
    return false;
}

static int
portal_open_pipewire_remote(struct helper *helper)
{
    DBusMessage *message, *reply;
    DBusMessageIter iter, options;
    const char *session_handle = helper->session_handle;
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

    reply = portal_call(helper->connection, message);
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
    struct helper_stream *stream = data;

    if (!param || id != SPA_PARAM_Format)
        return;

    spa_format_video_raw_parse(param, &stream->raw);
}

static void
stream_state_changed(void *data, enum pw_stream_state old,
                     enum pw_stream_state state, const char *error)
{
    struct helper_stream *stream = data;

    if (error || state == PW_STREAM_STATE_ERROR) {
        helper_log("Xwayland screencast helper: stream %u state %s -> %s%s%s",
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
    struct helper_stream *stream = data;
    struct helper *helper = stream->helper;
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
            helper_log("Xwayland screencast helper: ignoring unsupported "
                       "PipeWire buffer type=%u fd=%" PRId64,
                       spa_data->type, spa_data->fd);
            stream->logged_unsupported_buffer = true;
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
            helper_log("Xwayland screencast helper: ignoring short PipeWire "
                       "buffer available=%zu needed=%zu offset=%zu maxsize=%u "
                       "chunk-size=%u stride=%d height=%d",
                       available, needed, offset, spa_data->maxsize,
                       chunk->size, stride, height);
            stream->logged_short_buffer = true;
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
        if (!pread_full(spa_data->fd, frame + y * stride_size,
                        row_bytes,
                        (off_t) (fd_offset + y * stride_size))) {
            helper_log("Xwayland screencast helper: failed to read "
                       "PipeWire memfd buffer: %s", strerror(errno));
            free(frame);
            goto out;
        }
    }

    pthread_mutex_lock(&helper->mutex);
    free(stream->frame);
    stream->frame = frame;
    stream->frame_size = alloc_size;
    stream->frame_width = width;
    stream->frame_height = height;
    stream->frame_stride = stride;
    stream->frame_format = stream->raw.format;
    stream->frame_valid = true;
    if (stream->width == 0)
        stream->width = width;
    if (stream->height == 0)
        stream->height = height;
    helper->have_frame = true;
    pthread_cond_broadcast(&helper->cond);
    pthread_mutex_unlock(&helper->mutex);

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

static bool
pipewire_connect_streams(struct helper *helper, int fd)
{
    uint32_t i;
    bool ok = false;

    pw_init(NULL, NULL);

    helper->pw_loop = pw_thread_loop_new("xwayland-screencast-helper", NULL);
    if (!helper->pw_loop)
        return false;

    helper->pw_context =
        pw_context_new(pw_thread_loop_get_loop(helper->pw_loop), NULL, 0);
    if (!helper->pw_context)
        return false;

    helper->pw_core = pw_context_connect_fd(helper->pw_context, fd, NULL, 0);
    if (!helper->pw_core)
        return false;

    if (pw_thread_loop_start(helper->pw_loop) < 0)
        return false;

    pw_thread_loop_lock(helper->pw_loop);

    for (i = 0; i < helper->n_streams; i++) {
        struct helper_stream *stream = &helper->streams[i];
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

        stream->pw_stream = pw_stream_new(helper->pw_core,
                                          "xwayland-screencast-helper",
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
            helper_log("Xwayland screencast helper: failed to connect stream %u: %s",
                       stream->node_id, spa_strerror(ret));
            goto out_unlock;
        }

        pw_stream_set_active(stream->pw_stream, true);
    }

    ok = true;

out_unlock:
    pw_thread_loop_unlock(helper->pw_loop);
    return ok;
}

static void
pipewire_cleanup(struct helper *helper)
{
    uint32_t i;

    if (helper->pw_loop)
        pw_thread_loop_stop(helper->pw_loop);

    for (i = 0; i < helper->n_streams; i++) {
        struct helper_stream *stream = &helper->streams[i];

        if (stream->pw_stream)
            pw_stream_destroy(stream->pw_stream);
        free(stream->frame);
    }

    if (helper->pw_core)
        pw_core_disconnect(helper->pw_core);
    if (helper->pw_context)
        pw_context_destroy(helper->pw_context);
    if (helper->pw_loop)
        pw_thread_loop_destroy(helper->pw_loop);

    helper->pw_core = NULL;
    helper->pw_context = NULL;
    helper->pw_loop = NULL;

    free(helper->streams);
    helper->streams = NULL;
    helper->n_streams = 0;
}

static bool
portal_connect(struct helper *helper)
{
    DBusError error;
    int fd;

    helper_set_stage(helper, "connecting to session bus");
    dbus_error_init(&error);
    helper->connection = dbus_bus_get_private(DBUS_BUS_SESSION, &error);
    if (!helper->connection) {
        if (dbus_error_is_set(&error)) {
            helper_log("Xwayland screencast helper D-Bus session bus error: %s",
                       error.message);
            dbus_error_free(&error);
        }
        return false;
    }

    dbus_connection_set_exit_on_disconnect(helper->connection, false);
    if (helper_stopped(helper))
        return false;

    if (!portal_register_app_id(helper))
        return false;
    if (helper_stopped(helper))
        return false;

    helper_set_stage(helper, "subscribing to portal responses");
    helper->response_match = portal_add_response_match(helper->connection, NULL);
    if (!helper->response_match)
        return false;
    if (helper_stopped(helper))
        return false;

    helper_set_stage(helper, "creating portal session");
    helper->session_handle = portal_create_session(helper);
    if (!helper->session_handle)
        return false;
    if (helper_stopped(helper))
        return false;

    helper_set_stage(helper, "selecting portal sources");
    if (!portal_select_sources(helper))
        return false;
    if (helper_stopped(helper))
        return false;

    helper_set_stage(helper, "starting portal session");
    if (!portal_start(helper))
        return false;
    if (helper_stopped(helper))
        return false;

    helper_set_stage(helper, "opening PipeWire remote");
    fd = portal_open_pipewire_remote(helper);
    if (fd < 0)
        return false;
    if (helper_stopped(helper)) {
        close(fd);
        return false;
    }

    helper_set_stage(helper, "connecting PipeWire streams");
    if (!pipewire_connect_streams(helper, fd))
        return false;
    if (helper_stopped(helper))
        return false;

    helper_set_stage(helper, "waiting for PipeWire frames");
    return true;
}

static void
portal_cleanup(struct helper *helper)
{
    if (helper->connection && helper->session_handle) {
        DBusMessage *message;
        const char *session_handle = helper->session_handle;

        message = dbus_message_new_method_call(PORTAL_BUS_NAME,
                                               session_handle,
                                               "org.freedesktop.portal.Session",
                                               "Close");
        if (message) {
            DBusMessage *reply =
                portal_call_with_timeout(helper->connection, message,
                                         HELPER_DBUS_CLOSE_TIMEOUT_MS);

            if (reply)
                dbus_message_unref(reply);
        }
    }

    free(helper->session_handle);
    helper->session_handle = NULL;

    if (helper->connection) {
        if (helper->response_match) {
            dbus_bus_remove_match(helper->connection,
                                  helper->response_match, NULL);
            free(helper->response_match);
            helper->response_match = NULL;
        }

        dbus_connection_close(helper->connection);
        dbus_connection_unref(helper->connection);
        helper->connection = NULL;
    }
}

static void *
portal_thread(void *data)
{
    struct helper *helper = data;

    if (!portal_connect(helper)) {
        helper_set_failed(helper);
        goto out;
    }

    pthread_mutex_lock(&helper->mutex);
    while (!helper->stop)
        pthread_cond_wait(&helper->cond, &helper->mutex);
    pthread_mutex_unlock(&helper->mutex);

out:
    portal_cleanup(helper);
    pipewire_cleanup(helper);
    pthread_mutex_lock(&helper->mutex);
    helper->thread_finished = true;
    pthread_cond_broadcast(&helper->cond);
    pthread_mutex_unlock(&helper->mutex);
    return NULL;
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

static bool
helper_start_locked(struct helper *helper)
{
    if (helper->started)
        return true;

    helper->started = true;
    helper->thread_finished = false;
    if (pthread_create(&helper->thread, NULL, portal_thread, helper) != 0) {
        helper->started = false;
        helper->failed = true;
        pthread_cond_broadcast(&helper->cond);
        return false;
    }

    return true;
}

static bool
helper_ensure_frame(struct helper *helper)
{
    struct timespec deadline;
    bool ok;

    pthread_mutex_lock(&helper->mutex);

    if (helper->started && (helper->thread_finished || helper->stop)) {
        pthread_t thread = helper->thread;
        bool failed = helper->failed;

        pthread_mutex_unlock(&helper->mutex);
        pthread_join(thread, NULL);
        pthread_mutex_lock(&helper->mutex);

        helper->started = false;
        helper->thread_finished = false;
        helper->failed = false;
        helper->stop = false;
        helper->have_frame = false;
        helper->stage = failed ? "retrying after failed portal session" :
                                 "starting portal session";
        if (failed)
            helper_log("Xwayland screencast helper: retrying after failed portal session");
    }

    if (!helper_start_locked(helper)) {
        pthread_mutex_unlock(&helper->mutex);
        return false;
    }

    clock_gettime(CLOCK_REALTIME, &deadline);
    timespec_add_ms(&deadline, HELPER_START_TIMEOUT_MS);

    while (!helper->have_frame && !helper->failed && !helper->stop) {
        if (pthread_cond_timedwait(&helper->cond,
                                   &helper->mutex,
                                   &deadline) == ETIMEDOUT)
            break;
    }

    ok = helper->have_frame && !helper->failed;
    if (!ok) {
        helper_log("Xwayland screencast helper: ensure frame failed "
                   "started=%d finished=%d failed=%d stop=%d have_frame=%d stage=%s",
                   helper->started,
                   helper->thread_finished,
                   helper->failed,
                   helper->stop,
                   helper->have_frame,
                   helper->stage ? helper->stage : "unknown");
    }

    pthread_mutex_unlock(&helper->mutex);
    return ok;
}

static bool
helper_copy_frame(struct helper *helper,
                  struct xwl_screencast_helper_response *resp,
                  uint8_t **frame_out)
{
    uint32_t i;

    memset(resp, 0, sizeof *resp);
    resp->magic = XWL_SCREENCAST_HELPER_MAGIC;
    resp->version = XWL_SCREENCAST_HELPER_VERSION;
    resp->status = XWL_SCREENCAST_HELPER_STATUS_NO_FRAME;

    *frame_out = NULL;

    pthread_mutex_lock(&helper->mutex);
    for (i = 0; i < helper->n_streams; i++) {
        struct helper_stream *stream = &helper->streams[i];
        uint8_t *frame;

        if (!stream->frame_valid || !stream->frame || stream->frame_size == 0)
            continue;

        frame = malloc(stream->frame_size);
        if (!frame) {
            pthread_mutex_unlock(&helper->mutex);
            return false;
        }

        memcpy(frame, stream->frame, stream->frame_size);
        resp->status = XWL_SCREENCAST_HELPER_STATUS_OK;
        resp->format = stream->frame_format;
        resp->stream_x = stream->x;
        resp->stream_y = stream->y;
        resp->stream_width = stream->width;
        resp->stream_height = stream->height;
        if (resp->stream_width <= 0)
            resp->stream_width = stream->frame_width;
        if (resp->stream_height <= 0)
            resp->stream_height = stream->frame_height;
        resp->frame_width = stream->frame_width;
        resp->frame_height = stream->frame_height;
        resp->frame_stride = stream->frame_stride;
        resp->frame_size = stream->frame_size;
        *frame_out = frame;
        pthread_mutex_unlock(&helper->mutex);
        return true;
    }
    pthread_mutex_unlock(&helper->mutex);

    return true;
}

static void
helper_stop_and_join(struct helper *helper)
{
    pthread_t thread;
    bool started;

    pthread_mutex_lock(&helper->mutex);
    started = helper->started;
    if (started) {
        thread = helper->thread;
        helper->stop = true;
        pthread_cond_broadcast(&helper->cond);
    }
    pthread_mutex_unlock(&helper->mutex);

    if (!started)
        return;

    pthread_join(thread, NULL);

    pthread_mutex_lock(&helper->mutex);
    helper->started = false;
    helper->thread_finished = false;
    helper->failed = false;
    helper->stop = false;
    helper->have_frame = false;
    helper->stage = "starting portal session";
    pthread_mutex_unlock(&helper->mutex);
}

static void
handle_client(int fd)
{
    struct xwl_screencast_helper_request req;
    struct xwl_screencast_helper_response resp;
    struct helper *helper = NULL;
    uint8_t *frame = NULL;
    struct timeval timeout;
    uint32_t request_id;
    int64_t start_ms;
    const char *opcode_name;

    timeout.tv_sec = HELPER_START_TIMEOUT_MS / 1000;
    timeout.tv_usec = (HELPER_START_TIMEOUT_MS % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);

    if (!read_full(fd, &req, sizeof req))
        return;

    if (req.magic != XWL_SCREENCAST_HELPER_MAGIC ||
        req.version != XWL_SCREENCAST_HELPER_VERSION ||
        (req.opcode != XWL_SCREENCAST_HELPER_GET_FRAME &&
         req.opcode != XWL_SCREENCAST_HELPER_GET_SCREENCAST_FRAME)) {
        memset(&resp, 0, sizeof resp);
        resp.magic = XWL_SCREENCAST_HELPER_MAGIC;
        resp.version = XWL_SCREENCAST_HELPER_VERSION;
        resp.status = XWL_SCREENCAST_HELPER_STATUS_ERROR;
        write_full(fd, &resp, sizeof resp);
        return;
    }

    req.app_id[sizeof req.app_id - 1] = '\0';
    req.app_name[sizeof req.app_name - 1] = '\0';
    request_id = req.reserved ? req.reserved : helper_next_request_id();
    start_ms = helper_monotonic_msec();
    opcode_name = req.opcode == XWL_SCREENCAST_HELPER_GET_FRAME ?
        "screenshot" : "screencast";

    helper_log("Xwayland screenshot portal helper: request id=%u received "
               "opcode=%s app_id=%s app_name=%s",
               request_id, opcode_name, req.app_id, req.app_name);

    memset(&resp, 0, sizeof resp);
    resp.magic = XWL_SCREENCAST_HELPER_MAGIC;
    resp.version = XWL_SCREENCAST_HELPER_VERSION;
    resp.status = XWL_SCREENCAST_HELPER_STATUS_ERROR;

    if (req.opcode == XWL_SCREENCAST_HELPER_GET_FRAME) {
        const char *safe_app_id = helper_safe_app_id(req.app_id);
        const char *safe_app_name = helper_safe_app_name(req.app_name,
                                                         safe_app_id);

        helper = helper_new(safe_app_id, safe_app_name);
    } else {
        helper = helper_for_app_id(req.app_id, req.app_name);
    }
    if (!helper) {
        resp.status = XWL_SCREENCAST_HELPER_STATUS_ERROR;
    } else if (req.opcode == XWL_SCREENCAST_HELPER_GET_FRAME) {
        if (!portal_screenshot_get_frame(helper, request_id, fd,
                                         &resp, &frame))
            resp.status = XWL_SCREENCAST_HELPER_STATUS_ERROR;
    } else {
        if (!helper_ensure_frame(helper) ||
            !helper_copy_frame(helper, &resp, &frame))
            resp.status = XWL_SCREENCAST_HELPER_STATUS_ERROR;
    }

    helper_log("Xwayland screenshot portal helper: request id=%u sending "
               "response status=%u frame=%ux%u size=%" PRIu64
               " elapsed=%" PRId64 "ms",
               request_id, resp.status, resp.frame_width, resp.frame_height,
               resp.frame_size, helper_monotonic_msec() - start_ms);

    if (!write_full(fd, &resp, sizeof resp)) {
        helper_log("Xwayland screenshot portal helper: request id=%u failed "
                   "to write response header: %s",
                   request_id, strerror(errno));
        goto out;
    }

    if (resp.status == XWL_SCREENCAST_HELPER_STATUS_OK && frame) {
        if (!write_full(fd, frame, resp.frame_size)) {
            helper_log("Xwayland screenshot portal helper: request id=%u "
                       "failed to write frame payload: %s",
                       request_id, strerror(errno));
        } else {
            helper_log("Xwayland screenshot portal helper: request id=%u "
                       "finished frame payload elapsed=%" PRId64 "ms",
                       request_id, helper_monotonic_msec() - start_ms);
        }
    }

out:
    if (helper && req.opcode == XWL_SCREENCAST_HELPER_GET_SCREENCAST_FRAME)
        helper_stop_and_join(helper);
    if (helper && req.opcode == XWL_SCREENCAST_HELPER_GET_FRAME)
        helper_free_one(helper);
    free(frame);
}

static void *
client_thread(void *data)
{
    int fd = (int) (intptr_t) data;

    handle_client(fd);
    close(fd);
    return NULL;
}

static void
helper_stop_all(void)
{
    struct helper *helper;

    pthread_mutex_lock(&helpers_mutex);
    for (helper = helpers; helper; helper = helper->next) {
        pthread_mutex_lock(&helper->mutex);
        helper->stop = true;
        pthread_cond_broadcast(&helper->cond);
        pthread_mutex_unlock(&helper->mutex);
    }
    pthread_mutex_unlock(&helpers_mutex);

    for (helper = helpers; helper; helper = helper->next) {
        if (helper->started)
            pthread_join(helper->thread, NULL);
    }
}

static void
helper_free_all(void)
{
    struct helper *helper = helpers;

    while (helper) {
        struct helper *next = helper->next;

        helper_free_one(helper);
        helper = next;
    }

    helpers = NULL;
}

static int
get_systemd_listen_socket(void)
{
    const char *listen_pid_env = getenv("LISTEN_PID");
    const char *listen_fds_env = getenv("LISTEN_FDS");
    char *end = NULL;
    long listen_pid;
    long listen_fds;
    long i;
    int fd;

    if (!listen_pid_env || !listen_fds_env)
        return -1;

    errno = 0;
    listen_pid = strtol(listen_pid_env, &end, 10);
    if (errno != 0 || !end || *end != '\0' || listen_pid != getpid())
        return -1;

    errno = 0;
    end = NULL;
    listen_fds = strtol(listen_fds_env, &end, 10);
    if (errno != 0 || !end || *end != '\0' || listen_fds < 1)
        return -1;

    unsetenv("LISTEN_PID");
    unsetenv("LISTEN_FDS");
    unsetenv("LISTEN_FDNAMES");

    fd = SD_LISTEN_FDS_START;
    set_close_on_exec(fd);

    for (i = 1; i < listen_fds; i++)
        close(SD_LISTEN_FDS_START + i);

    helper_log("Xwayland screencast helper: using systemd socket "
               "activation fd %d", fd);
    return fd;
}

static int
create_abstract_listen_socket(void)
{
    struct sockaddr_un addr;
    char name[sizeof(addr.sun_path) - 1];
    size_t name_len;
    socklen_t addr_len;
    int fd;

    snprintf(name, sizeof name, "%s%lu",
             XWL_SCREENCAST_HELPER_SOCKET_PREFIX, (unsigned long) getuid());
    name_len = strlen(name);
    if (name_len + 1 > sizeof(addr.sun_path)) {
        helper_log("Xwayland screencast helper: abstract socket name too long");
        return -1;
    }

    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;

    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    addr.sun_path[0] = '\0';
    memcpy(addr.sun_path + 1, name, name_len);
    addr_len = offsetof(struct sockaddr_un, sun_path) + 1 + name_len;

    if (bind(fd, (struct sockaddr *) &addr, addr_len) < 0) {
        if (errno == EADDRINUSE)
            helper_log("Xwayland screencast helper: socket %s already exists",
                       name);
        else
            helper_log("Xwayland screencast helper: failed to bind %s: %s",
                       name, strerror(errno));
        close(fd);
        return -1;
    }

    if (listen(fd, 16) < 0) {
        helper_log("Xwayland screencast helper: listen failed: %s",
                   strerror(errno));
        close(fd);
        return -1;
    }

    helper_log("Xwayland screencast helper: listening on abstract socket %s",
               name);
    return fd;
}

static int
create_path_listen_socket(void)
{
    struct sockaddr_un addr;
    int fd;

    snprintf(listen_path, sizeof listen_path, "%s%lu%s",
             XWL_SCREENCAST_HELPER_PATH_PREFIX,
             (unsigned long) getuid(),
             XWL_SCREENCAST_HELPER_PATH_SUFFIX);
    if (strlen(listen_path) >= sizeof addr.sun_path) {
        helper_log("Xwayland screencast helper: path socket name too long");
        listen_path[0] = '\0';
        return -1;
    }

    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;

    unlink(listen_path);

    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, listen_path, strlen(listen_path) + 1);

    if (bind(fd, (struct sockaddr *) &addr, sizeof addr) < 0) {
        helper_log("Xwayland screencast helper: failed to bind %s: %s",
                   listen_path, strerror(errno));
        close(fd);
        listen_path[0] = '\0';
        return -1;
    }

    if (chmod(listen_path, 0666) < 0)
        helper_log("Xwayland screencast helper: failed to chmod %s: %s",
                   listen_path, strerror(errno));

    if (listen(fd, 16) < 0) {
        helper_log("Xwayland screencast helper: listen failed: %s",
                   strerror(errno));
        close(fd);
        unlink(listen_path);
        listen_path[0] = '\0';
        return -1;
    }

    helper_log("Xwayland screencast helper: listening on path socket %s",
               listen_path);
    return fd;
}

static int
create_listen_socket(void)
{
    int fd;

    fd = get_systemd_listen_socket();
    if (fd >= 0)
        return fd;

    fd = create_abstract_listen_socket();
    if (fd >= 0)
        return fd;

    helper_log("Xwayland screencast helper: falling back to /tmp socket");
    return create_path_listen_socket();
}

static void
signal_handler(int signal)
{
    (void) signal;
    stop_requested = 1;
}

static void
install_signal_handlers(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof action);
    action.sa_handler = signal_handler;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);

    memset(&action, 0, sizeof action);
    action.sa_handler = SIG_IGN;
    sigemptyset(&action.sa_mask);
    sigaction(SIGPIPE, &action, NULL);
}

int
main(int argc, char **argv)
{
    int listen_fd;

    install_signal_handlers();
    dbus_threads_init_default();

    listen_fd = create_listen_socket();
    if (listen_fd < 0)
        return 1;

    if (!set_nonblocking(listen_fd)) {
        helper_log("Xwayland screencast helper: failed to set listen socket "
                   "nonblocking: %s", strerror(errno));
        close(listen_fd);
        if (listen_path[0] != '\0') {
            unlink(listen_path);
            listen_path[0] = '\0';
        }
        return 1;
    }

    while (!stop_requested) {
        struct pollfd pfd = {
            .fd = listen_fd,
            .events = POLLIN,
        };
        int ready;

        ready = poll(&pfd, 1, HELPER_ACCEPT_POLL_TIMEOUT_MS);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            helper_log("Xwayland screencast helper: poll failed: %s",
                       strerror(errno));
            break;
        }
        if (ready == 0)
            continue;
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            helper_log("Xwayland screencast helper: listen socket closed "
                       "while waiting for clients, revents=0x%x",
                       pfd.revents);
            break;
        }
        if (!(pfd.revents & POLLIN))
            continue;

        for (;;) {
            pthread_t thread;
            int ret;
            int fd = accept(listen_fd, NULL, NULL);

            if (fd < 0) {
                if (errno == EINTR)
                    continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                helper_log("Xwayland screencast helper: accept failed: %s",
                           strerror(errno));
                stop_requested = 1;
                break;
            }

            set_close_on_exec(fd);
            ret = pthread_create(&thread, NULL, client_thread,
                                 (void *) (intptr_t) fd);
            if (ret == 0) {
                pthread_detach(thread);
            } else {
                helper_log("Xwayland screencast helper: failed to create "
                           "client thread: %s", strerror(ret));
                handle_client(fd);
                close(fd);
            }
        }
    }

    close(listen_fd);
    if (listen_path[0] != '\0') {
        unlink(listen_path);
        listen_path[0] = '\0';
    }

    helper_stop_all();
    helper_free_all();
    return 0;
}
