/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * window-state.h - Remembered window size and page zoom
 *
 * Copyright (C) 2026 averagenative
 */
#ifndef GHD_WINDOW_STATE_H
#define GHD_WINDOW_STATE_H

#include <glib.h>

/*
 * Wayland does not let a client place its own window, so unlike the
 * Electron build this keeps size and maximized state, not position.
 */
typedef struct {
	gint     width;
	gint     height;
	gboolean maximized;
	gdouble  zoom;
} GhdWindowState;

/* Fill @state from <state dir>/window.ini, or with defaults. */
void ghd_window_state_load(GhdWindowState *state);

/* Write @state to <state dir>/window.ini. */
void ghd_window_state_save(const GhdWindowState *state);

#endif /* GHD_WINDOW_STATE_H */
