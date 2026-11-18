/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * firefox-import.c - Copy the Google session out of a Firefox profile
 *
 * Copyright (C) 2026 averagenative
 *
 * Google may refuse to sign in from an embedded browser. The fallback
 * is to sign in with Firefox, then copy its *.google.com cookies into
 * this app's WebKit cookie store.
 */
#include <sqlite3.h>
#include <glib/gstdio.h>

#include "firefox-import.h"
#include "ghd.h"

G_DEFINE_QUARK(ghd-firefox-import-error-quark, ghd_firefox_import_error)
#define GHD_IMPORT_ERROR (ghd_firefox_import_error_quark())

enum {
	GHD_IMPORT_ERROR_NO_PROFILE,
	GHD_IMPORT_ERROR_DATABASE,
	GHD_IMPORT_ERROR_NO_SESSION,
};

/* moz_cookies.sameSite */
enum {
	FIREFOX_SAMESITE_NONE   = 0,
	FIREFOX_SAMESITE_LAX    = 1,
	FIREFOX_SAMESITE_STRICT = 2,
};

static const gchar *const db_files[] = { "cookies.sqlite", "cookies.sqlite-wal" };

/* --- finding the profile --- */

/*
 * When Firefox last wrote the profile's cookie store, or -1 if it has
 * none. Firefox writes to the write-ahead log first, so that file's
 * time counts too.
 */
static gint64
cookies_mtime(const gchar *profile)
{
	gint64 newest = -1;

	for (guint i = 0; i < G_N_ELEMENTS(db_files); i++) {
		g_autofree gchar *path = g_build_filename(profile, db_files[i], NULL);
		GStatBuf st;

		if (g_stat(path, &st) != 0) {
			if (i == 0)
				return -1;
			continue;
		}
		newest = MAX(newest, (gint64)st.st_mtime);
	}
	return newest;
}

/*
 * The profile with the most recently written cookie store. Firefox
 * lists profiles in profiles.ini under ~/.mozilla/firefox, or under
 * $XDG_CONFIG_HOME/mozilla/firefox in newer releases.
 */
static gchar *
find_newest_profile(GError **error)
{
	g_autofree gchar *legacy = g_build_filename(g_get_home_dir(), ".mozilla", "firefox", NULL);
	g_autofree gchar *xdg = g_build_filename(g_get_user_config_dir(), "mozilla", "firefox", NULL);
	const gchar *roots[] = { legacy, xdg };
	gchar *best = NULL;
	gint64 best_mtime = -1;

	for (guint r = 0; r < G_N_ELEMENTS(roots); r++) {
		g_autoptr(GKeyFile) ini = g_key_file_new();
		g_autofree gchar *ini_path = g_build_filename(roots[r], "profiles.ini", NULL);
		g_auto(GStrv) groups = NULL;

		if (!g_key_file_load_from_file(ini, ini_path, G_KEY_FILE_NONE, NULL))
			continue;
		groups = g_key_file_get_groups(ini, NULL);
		for (guint i = 0; groups[i] != NULL; i++) {
			g_autofree gchar *path = g_key_file_get_string(ini, groups[i], "Path", NULL);
			g_autofree gchar *dir = NULL;
			gint64 mtime;

			if (path == NULL)
				continue;
			/* IsRelative defaults to 1; 0 means Path is absolute. */
			if (g_key_file_has_key(ini, groups[i], "IsRelative", NULL) &&
			    g_key_file_get_integer(ini, groups[i], "IsRelative", NULL) == 0)
				dir = g_strdup(path);
			else
				dir = g_build_filename(roots[r], path, NULL);

			mtime = cookies_mtime(dir);
			if (mtime > best_mtime) {
				g_free(best);
				best = g_steal_pointer(&dir);
				best_mtime = mtime;
			}
		}
	}

	if (best == NULL)
		g_set_error_literal(error, GHD_IMPORT_ERROR, GHD_IMPORT_ERROR_NO_PROFILE,
			"No Firefox profile with a cookie database was found.");
	return best;
}

/* --- reading a private copy of the database --- */

static void
remove_tmpdir(const gchar *dir)
{
	g_autoptr(GDir) d = g_dir_open(dir, 0, NULL);
	const gchar *name;

	if (d != NULL) {
		while ((name = g_dir_read_name(d)) != NULL) {
			g_autofree gchar *path = g_build_filename(dir, name, NULL);

			g_unlink(path);
		}
	}
	g_rmdir(dir);
}

/*
 * Firefox keeps its cookie database open while it runs, and the newest
 * rows may still be in the write-ahead log. Copy both files into an
 * owner-only temporary directory and open the copy: SQLite replays the
 * log when it opens it.
 */
static sqlite3 *
open_copy(const gchar *profile, gchar **tmpdir_out, GError **error)
{
	g_autofree gchar *tmpdir = g_dir_make_tmp("google-home-import-XXXXXX", error);
	g_autofree gchar *db_path = NULL;
	sqlite3 *db = NULL;

	if (tmpdir == NULL)
		return NULL;

	for (guint i = 0; i < G_N_ELEMENTS(db_files); i++) {
		g_autofree gchar *src_path = g_build_filename(profile, db_files[i], NULL);
		g_autofree gchar *dst_path = g_build_filename(tmpdir, db_files[i], NULL);
		g_autoptr(GFile) src = g_file_new_for_path(src_path);
		g_autoptr(GFile) dst = g_file_new_for_path(dst_path);

		if (i > 0 && !g_file_test(src_path, G_FILE_TEST_EXISTS))
			continue;
		if (!g_file_copy(src, dst, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, error)) {
			remove_tmpdir(tmpdir);
			return NULL;
		}
	}

	db_path = g_build_filename(tmpdir, db_files[0], NULL);
	if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) {
		g_set_error(error, GHD_IMPORT_ERROR, GHD_IMPORT_ERROR_DATABASE,
			"Cannot open the Firefox cookie database: %s", sqlite3_errmsg(db));
		sqlite3_close(db);
		remove_tmpdir(tmpdir);
		return NULL;
	}
	*tmpdir_out = g_steal_pointer(&tmpdir);
	return db;
}

/*
 * Warn when Firefox has not used its Google session for a while: Google
 * may have rotated it, and the import then lands on a sign-in page.
 */
static void
log_session_age(sqlite3 *db)
{
	static const char sql[] =
		"SELECT MAX(lastAccessed) FROM moz_cookies"
		" WHERE host = '.google.com' AND name = '__Secure-1PSIDTS'"
		" AND originAttributes = ''";
	sqlite3_stmt *stmt = NULL;

	if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
		return;
	if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
		/* lastAccessed is in microseconds. */
		gint64 last = sqlite3_column_int64(stmt, 0);
		gdouble days = (gdouble)(g_get_real_time() - last) / G_USEC_PER_SEC / 86400.0;

		if (days > 1.0)
			ghd_log("warning: Firefox last used this Google session %.0f days ago. "
				"If the app lands on a sign-in or cookie error page, open "
				"https://home.google.com in Firefox first, then import again.", days);
	}
	sqlite3_finalize(stmt);
}

static SoupCookie *
cookie_from_row(sqlite3_stmt *stmt, gint64 now)
{
	const gchar *host = (const gchar *)sqlite3_column_text(stmt, 0);
	const gchar *path = (const gchar *)sqlite3_column_text(stmt, 1);
	const gchar *name = (const gchar *)sqlite3_column_text(stmt, 2);
	const gchar *value = (const gchar *)sqlite3_column_text(stmt, 3);
	gint64 expiry = sqlite3_column_int64(stmt, 6);
	g_autoptr(GDateTime) expires = NULL;
	SoupCookie *cookie;

	if (host == NULL || *host == '\0' || path == NULL || name == NULL || value == NULL)
		return NULL;

	/* Newer Firefox stores expiry in milliseconds, older in seconds. */
	if (expiry > G_GINT64_CONSTANT(100000000000))
		expiry /= 1000;
	if (expiry <= now)
		return NULL;

	/*
	 * Firefox and libsoup spell cookie scope the same way: a domain
	 * cookie's host starts with a dot, a host-only cookie's (every
	 * __Host- cookie among them) does not. Keeping that distinction
	 * matters: widening accounts.google.com to .accounts.google.com
	 * gives Google two different LSIDs, and it answers with
	 * CookieMismatch.
	 */
	cookie = soup_cookie_new(name, value, host, path, -1);
	expires = g_date_time_new_from_unix_utc(expiry);
	if (expires != NULL)
		soup_cookie_set_expires(cookie, expires);
	soup_cookie_set_secure(cookie, sqlite3_column_int(stmt, 4) != 0);
	soup_cookie_set_http_only(cookie, sqlite3_column_int(stmt, 5) != 0);
	switch (sqlite3_column_int(stmt, 7)) {
	case FIREFOX_SAMESITE_NONE:
		soup_cookie_set_same_site_policy(cookie, SOUP_SAME_SITE_POLICY_NONE);
		break;
	case FIREFOX_SAMESITE_LAX:
		soup_cookie_set_same_site_policy(cookie, SOUP_SAME_SITE_POLICY_LAX);
		break;
	case FIREFOX_SAMESITE_STRICT:
		soup_cookie_set_same_site_policy(cookie, SOUP_SAME_SITE_POLICY_STRICT);
		break;
	default:
		/* "Unset": keep libsoup's default. */
		break;
	}
	return cookie;
}

static GList *
read_rows(sqlite3 *db, GError **error)
{
	/*
	 * originAttributes = '' keeps first-party cookies outside containers
	 * and private windows. Partitioned (CHIPS) copies set while browsing
	 * other sites would otherwise overwrite the real ones.
	 */
	static const char sql[] =
		"SELECT host, path, name, value, isSecure, isHttpOnly, expiry, sameSite"
		" FROM moz_cookies"
		" WHERE (host = 'google.com' OR host LIKE '%.google.com')"
		" AND originAttributes = ''";
	sqlite3_stmt *stmt = NULL;
	gint64 now = g_get_real_time() / G_USEC_PER_SEC;
	gboolean signed_in = FALSE;
	GList *cookies = NULL;
	int rc;

	if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
		g_set_error(error, GHD_IMPORT_ERROR, GHD_IMPORT_ERROR_DATABASE,
			"Cannot read the Firefox cookie database: %s", sqlite3_errmsg(db));
		return NULL;
	}
	while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
		SoupCookie *cookie = cookie_from_row(stmt, now);
		const gchar *name;

		if (cookie == NULL)
			continue;
		name = soup_cookie_get_name(cookie);
		if (g_str_equal(name, "SID") || g_str_equal(name, "__Secure-1PSID"))
			signed_in = TRUE;
		cookies = g_list_prepend(cookies, cookie);
	}
	if (rc != SQLITE_DONE) {
		g_set_error(error, GHD_IMPORT_ERROR, GHD_IMPORT_ERROR_DATABASE,
			"Cannot read the Firefox cookie database: %s", sqlite3_errmsg(db));
		g_list_free_full(cookies, (GDestroyNotify)soup_cookie_free);
		cookies = NULL;
	} else if (!signed_in) {
		g_set_error_literal(error, GHD_IMPORT_ERROR, GHD_IMPORT_ERROR_NO_SESSION,
			"Firefox is not signed in to Google. Sign in to "
			"https://home.google.com in Firefox first.");
		g_list_free_full(cookies, (GDestroyNotify)soup_cookie_free);
		cookies = NULL;
	}
	sqlite3_finalize(stmt);
	return g_list_reverse(cookies);
}

GList *
ghd_firefox_read_google_cookies(const gchar *profile_dir, gchar **profile_used, GError **error)
{
	g_autofree gchar *profile = NULL;
	g_autofree gchar *tmpdir = NULL;
	sqlite3 *db;
	GList *cookies;

	profile = profile_dir != NULL ? g_strdup(profile_dir) : find_newest_profile(error);
	if (profile == NULL)
		return NULL;
	if (cookies_mtime(profile) < 0) {
		g_set_error(error, GHD_IMPORT_ERROR, GHD_IMPORT_ERROR_NO_PROFILE,
			"%s has no Firefox cookie database (cookies.sqlite).", profile);
		return NULL;
	}

	db = open_copy(profile, &tmpdir, error);
	if (db == NULL)
		return NULL;
	log_session_age(db);
	cookies = read_rows(db, error);
	sqlite3_close(db);
	remove_tmpdir(tmpdir);

	if (cookies != NULL && profile_used != NULL)
		*profile_used = g_steal_pointer(&profile);
	return cookies;
}

/* --- writing them into WebKit --- */

gboolean
ghd_cookie_is_google(SoupCookie *cookie)
{
	const gchar *domain = soup_cookie_get_domain(cookie);

	if (domain == NULL)
		return FALSE;
	if (*domain == '.')
		domain++;
	return g_ascii_strcasecmp(domain, "google.com") == 0 ||
	       g_str_has_suffix(domain, ".google.com");
}

typedef struct {
	GList *imported;   /* SoupCookie, from Firefox */
	GList *merged;     /* SoupCookie, what replaces the store */
	guint  n_imported;
} ImportData;

static void
import_data_free(gpointer data)
{
	ImportData *d = data;

	g_list_free_full(d->imported, (GDestroyNotify)soup_cookie_free);
	g_list_free_full(d->merged, (GDestroyNotify)soup_cookie_free);
	g_free(d);
}

static void
cookies_replaced(GObject *source, GAsyncResult *result, gpointer user_data)
{
	GTask *task = user_data;
	ImportData *d = g_task_get_task_data(task);
	GError *error = NULL;

	if (webkit_cookie_manager_replace_cookies_finish(WEBKIT_COOKIE_MANAGER(source), result, &error))
		g_task_return_int(task, d->n_imported);
	else
		g_task_return_error(task, error);
	g_object_unref(task);
}

static void
got_all_cookies(GObject *source, GAsyncResult *result, gpointer user_data)
{
	GTask *task = user_data;
	ImportData *d = g_task_get_task_data(task);
	GError *error = NULL;
	GList *existing;
	guint cleared = 0;

	existing = webkit_cookie_manager_get_all_cookies_finish(WEBKIT_COOKIE_MANAGER(source),
		result, &error);
	if (error != NULL) {
		g_task_return_error(task, error);
		g_object_unref(task);
		return;
	}

	/*
	 * Replace the old Google session wholesale. Mixing fresh cookies
	 * with leftovers from a stale session leaves conflicting copies, and
	 * Google answers that with accounts.google.com/CookieMismatch.
	 */
	for (GList *l = existing; l != NULL; l = l->next) {
		if (ghd_cookie_is_google(l->data)) {
			soup_cookie_free(l->data);
			cleared++;
		} else {
			d->merged = g_list_prepend(d->merged, l->data);
		}
	}
	g_list_free(existing);
	d->merged = g_list_concat(g_list_reverse(d->merged), g_steal_pointer(&d->imported));

	ghd_log("cookie import: replacing %u Google cookies with %u from Firefox",
		cleared, d->n_imported);
	webkit_cookie_manager_replace_cookies(WEBKIT_COOKIE_MANAGER(source), d->merged,
		g_task_get_cancellable(task), cookies_replaced, task);
}

void
ghd_firefox_import_async(WebKitCookieManager *cookies, const gchar *profile_dir,
                         GCancellable *cancellable, GAsyncReadyCallback callback,
                         gpointer user_data)
{
	GTask *task = g_task_new(cookies, cancellable, callback, user_data);
	g_autofree gchar *profile = NULL;
	GError *error = NULL;
	ImportData *d;
	GList *imported;

	g_task_set_source_tag(task, ghd_firefox_import_async);

	/* A copy and one query over a few hundred rows: quick enough to run here. */
	imported = ghd_firefox_read_google_cookies(profile_dir, &profile, &error);
	if (imported == NULL) {
		g_task_return_error(task, error);
		g_object_unref(task);
		return;
	}
	ghd_log("read %u Google cookies from %s", g_list_length(imported), profile);

	d = g_new0(ImportData, 1);
	d->imported = imported;
	d->n_imported = g_list_length(imported);
	g_task_set_task_data(task, d, import_data_free);
	webkit_cookie_manager_get_all_cookies(cookies, cancellable, got_all_cookies, task);
}

gboolean
ghd_firefox_import_finish(WebKitCookieManager *cookies, GAsyncResult *result,
                          guint *n_imported, GError **error)
{
	gssize n;

	g_return_val_if_fail(g_task_is_valid(result, cookies), FALSE);

	n = g_task_propagate_int(G_TASK(result), error);
	if (n < 0)
		return FALSE;
	if (n_imported != NULL)
		*n_imported = (guint)n;
	return TRUE;
}
