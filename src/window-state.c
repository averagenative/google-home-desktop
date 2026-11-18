/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * window-state.c - Remembered window size and page zoom
 *
 * Copyright (C) 2026 averagenative
 */
#include "window-state.h"
#include "ghd.h"

#define GROUP "window"

#define DEFAULT_WIDTH  1200
#define DEFAULT_HEIGHT 800

static gchar *
state_path(void)
{
	return g_build_filename(ghd_state_dir(), "window.ini", NULL);
}

void
ghd_window_state_load(GhdWindowState *state)
{
	g_autoptr(GKeyFile) kf = g_key_file_new();
	g_autofree gchar *path = state_path();
	gint width, height;
	gdouble zoom;

	state->width = DEFAULT_WIDTH;
	state->height = DEFAULT_HEIGHT;
	state->maximized = FALSE;
	state->zoom = 1.0;

	if (!g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL))
		return;

	/* A missing key reads as 0 and fails the range checks below. */
	width = g_key_file_get_integer(kf, GROUP, "width", NULL);
	height = g_key_file_get_integer(kf, GROUP, "height", NULL);
	if (width >= 320 && width <= 16384 && height >= 240 && height <= 16384) {
		state->width = width;
		state->height = height;
	}
	state->maximized = g_key_file_get_boolean(kf, GROUP, "maximized", NULL);
	zoom = g_key_file_get_double(kf, GROUP, "zoom", NULL);
	if (zoom >= 0.25 && zoom <= 5.0)
		state->zoom = zoom;
}

void
ghd_window_state_save(const GhdWindowState *state)
{
	g_autoptr(GKeyFile) kf = g_key_file_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = state_path();
	gchar zoom[G_ASCII_DTOSTR_BUF_SIZE];

	g_key_file_set_integer(kf, GROUP, "width", state->width);
	g_key_file_set_integer(kf, GROUP, "height", state->height);
	g_key_file_set_boolean(kf, GROUP, "maximized", state->maximized);
	/* Two decimals, whatever the locale's decimal separator. */
	g_key_file_set_value(kf, GROUP, "zoom", g_ascii_formatd(zoom, sizeof zoom, "%.2f", state->zoom));
	if (!g_key_file_save_to_file(kf, path, &error))
		ghd_log("cannot save window state: %s", error->message);
}
