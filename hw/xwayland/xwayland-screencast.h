/*
 * Copyright (C) 2026
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation.
 */

#ifndef XWAYLAND_SCREENCAST_H
#define XWAYLAND_SCREENCAST_H

#include <xwayland-config.h>

#include <dix.h>

#include "xwayland-screen.h"

Bool xwl_screencast_init(struct xwl_screen *xwl_screen);
void xwl_screencast_fini(struct xwl_screen *xwl_screen);
Bool xwl_screencast_prepare_image(struct xwl_screen *xwl_screen,
                                  ClientPtr client,
                                  DrawablePtr drawable);
void xwl_screencast_cancel_image(struct xwl_screen *xwl_screen,
                                 ClientPtr client);
Bool xwl_screencast_get_image(struct xwl_screen *xwl_screen,
                              ClientPtr client,
                              DrawablePtr drawable,
                              int x, int y, int width, int height,
                              unsigned int format,
                              unsigned long plane_mask,
                              char *dst);
Bool xwl_screencast_copy_area(struct xwl_screen *xwl_screen,
                              ClientPtr client,
                              DrawablePtr src, DrawablePtr dst,
                              GCPtr gc,
                              int src_x, int src_y,
                              int width, int height,
                              int dst_x, int dst_y,
                              RegionPtr *exposed);
Bool xwl_screencast_name_window_pixmap(struct xwl_screen *xwl_screen,
                                       ClientPtr client,
                                       WindowPtr window,
                                       PixmapPtr pixmap,
                                       CARD32 pixmap_id);

#endif /* XWAYLAND_SCREENCAST_H */
