/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * log.c - A small append-only log next to the window state
 *
 * Copyright (C) 2026 averagenative
 */
#include <stdarg.h>
#include <stdio.h>

#include <glib/gstdio.h>

#include "ghd.h"

/* Past this size the log moves to main.log.old at startup. */
#define LOG_ROTATE_BYTES (1024 * 1024)

static gchar *log_path;

const gchar *
ghd_state_dir(void)
{
	static gchar *dir;

	if (dir == NULL) {
		dir = g_build_filename(g_get_user_state_dir(), GHD_APP_ID, NULL);
		if (g_mkdir_with_parents(dir, 0700) != 0)
			g_printerr("google-home: cannot create %s\n", dir);
	}
	return dir;
}

void
ghd_log_init(void)
{
	GStatBuf st;

	log_path = g_build_filename(ghd_state_dir(), "main.log", NULL);
	if (g_stat(log_path, &st) == 0 && st.st_size > LOG_ROTATE_BYTES) {
		g_autofree gchar *old = g_strconcat(log_path, ".old", NULL);

		g_rename(log_path, old);
	}
}

void
ghd_log(const gchar *format, ...)
{
	g_autoptr(GDateTime) now = g_date_time_new_now_utc();
	g_autofree gchar *stamp = g_date_time_format_iso8601(now);
	g_autofree gchar *message = NULL;
	g_autofree gchar *line = NULL;
	va_list args;
	FILE *f;

	va_start(args, format);
	message = g_strdup_vprintf(format, args);
	va_end(args);
	line = g_strdup_printf("[%s] %s\n", stamp, message);

	fputs(line, stderr);
	if (log_path != NULL && (f = g_fopen(log_path, "a")) != NULL) {
		fputs(line, f);
		fclose(f);
	}
}
