/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * ghd.h - Shared definitions for Google Home Desktop
 *
 * Copyright (C) 2026 averagenative
 */
#ifndef GHD_H
#define GHD_H

#include <glib.h>

/*
 * The program name. WebKit derives the default network session's data
 * and cache directories from it (~/.local/share/google-home-desktop,
 * ~/.cache/google-home-desktop), and GTK uses it as the Wayland app_id
 * and X11 WM_CLASS. The installed .desktop file has the same name, so
 * the shell matches the window to its launcher.
 */
#define GHD_APP_ID   "google-home-desktop"
#define GHD_APP_NAME "Google Home"

/*
 * The bare home.google.com is a marketing page whose "Go to Google Home
 * web" button points at /login, so start there. With a live session it
 * goes straight into the app.
 */
#define GHD_HOME_URL "https://home.google.com/login"

/* ~/.local/state/google-home-desktop, created owner-only on first use. */
const gchar *ghd_state_dir(void);

/* Log to stderr and to <state dir>/main.log. */
void ghd_log_init(void);
void ghd_log(const gchar *format, ...) G_GNUC_PRINTF(1, 2);

#endif /* GHD_H */
