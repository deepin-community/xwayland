/*
 * Copyright (C) 2026
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation.
 */

#ifndef XWAYLAND_SCREENCAST_PROTOCOL_H
#define XWAYLAND_SCREENCAST_PROTOCOL_H

#include <stdint.h>

#define XWL_SCREENCAST_HELPER_SOCKET_PREFIX "xwayland-screencast-portal."
#define XWL_SCREENCAST_HELPER_PATH_PREFIX "/tmp/xwayland-screencast-portal-"
#define XWL_SCREENCAST_HELPER_PATH_SUFFIX ".sock"
#define XWL_SCREENCAST_HELPER_MAGIC 0x58475343u /* XGSC */
#define XWL_SCREENCAST_HELPER_VERSION 2u
#define XWL_SCREENCAST_HELPER_APP_ID_SIZE 256u
#define XWL_SCREENCAST_HELPER_APP_NAME_SIZE 128u

enum xwl_screencast_helper_opcode {
    XWL_SCREENCAST_HELPER_GET_FRAME = 1,
    XWL_SCREENCAST_HELPER_GET_SCREENCAST_FRAME = 2,
};

enum xwl_screencast_helper_status {
    XWL_SCREENCAST_HELPER_STATUS_OK = 0,
    XWL_SCREENCAST_HELPER_STATUS_ERROR = 1,
    XWL_SCREENCAST_HELPER_STATUS_NO_FRAME = 2,
};

struct xwl_screencast_helper_request {
    uint32_t magic;
    uint32_t version;
    uint32_t opcode;
    uint32_t reserved;
    char app_id[XWL_SCREENCAST_HELPER_APP_ID_SIZE];
    char app_name[XWL_SCREENCAST_HELPER_APP_NAME_SIZE];
};

struct xwl_screencast_helper_response {
    uint32_t magic;
    uint32_t version;
    uint32_t status;
    uint32_t format;
    int32_t stream_x;
    int32_t stream_y;
    int32_t stream_width;
    int32_t stream_height;
    uint32_t frame_width;
    uint32_t frame_height;
    uint32_t frame_stride;
    uint32_t reserved;
    uint64_t frame_size;
};

#endif /* XWAYLAND_SCREENCAST_PROTOCOL_H */
