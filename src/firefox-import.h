/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * firefox-import.h - Copy the Google session out of a Firefox profile
 *
 * Copyright (C) 2026 averagenative
 */
#ifndef GHD_FIREFOX_IMPORT_H
#define GHD_FIREFOX_IMPORT_H

#include <webkit/webkit.h>

G_BEGIN_DECLS

/*
 * Read the signed-in google.com cookies of a Firefox profile.
 * @profile_dir: a profile directory, or %NULL for the profile whose
 *   cookie store Firefox wrote most recently
 * @profile_used: (out) (optional): the profile that was read
 *
 * Fails when no profile is found, the database cannot be read, or the
 * profile holds no signed-in Google session.
 *
 * Returns: (transfer full) (element-type SoupCookie): the cookies, or
 *   %NULL with @error set
 */
GList   *ghd_firefox_read_google_cookies(const gchar *profile_dir,
                                         gchar **profile_used,
                                         GError **error);

/*
 * Replace every google.com cookie in @cookies' store with the Firefox
 * profile's (see ghd_firefox_read_google_cookies()). Cookies for other
 * sites are kept.
 */
void     ghd_firefox_import_async(WebKitCookieManager *cookies,
                                  const gchar *profile_dir,
                                  GCancellable *cancellable,
                                  GAsyncReadyCallback callback,
                                  gpointer user_data);

/* @n_imported: (out) (optional): how many cookies were added */
gboolean ghd_firefox_import_finish(WebKitCookieManager *cookies,
                                   GAsyncResult *result,
                                   guint *n_imported,
                                   GError **error);

/* Whether @cookie belongs to google.com or one of its subdomains. */
gboolean ghd_cookie_is_google(SoupCookie *cookie);

G_END_DECLS

#endif /* GHD_FIREFOX_IMPORT_H */
