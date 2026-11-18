/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * main.c - Google Home Desktop: home.google.com in a libgsurf window
 *
 * Copyright (C) 2026 averagenative
 *
 * A thin host around libgsurf (WebKitGTK 6.0, GTK4): one window on
 * home.google.com with a persistent session. Google's sign-in and
 * account pages stay in the window; every other site opens in the
 * system browser. If Google refuses to sign you in here, the session
 * can be imported from Firefox instead.
 */
#include <signal.h>
#include <string.h>

#include <glib-unix.h>
#include <gtk/gtk.h>
#include <webkit/webkit.h>

#include <gsurf.h>
#include <control/gsurf-control.h>

#include "firefox-import.h"
#include "ghd.h"
#include "window-state.h"

#define CONTROL_NAME "google-home"
#define ZOOM_STEP    1.1
#define CRASH_RELOAD_INTERVAL (30 * G_USEC_PER_SEC)

typedef enum {
	IMPORT_STARTUP,    /* no session at launch: failures are only logged */
	IMPORT_REQUESTED,  /* --import-firefox: failures are shown */
	IMPORT_PROMPTED,   /* the signed-out dialog: failures are shown */
} ImportMode;

static struct {
	GsurfApplication   *app;
	GsurfWindow        *window;   /* the main window, NULL once closed */
	GsurfView          *view;     /* its only view, NULL once closed */
	GsurfControlServer *control;
	GhdWindowState      state;
	gboolean            offered_import;
	gint64              last_crash_reload;
} G;

static gboolean  opt_import_firefox;
static gchar    *opt_firefox_profile;
static gboolean  opt_check_firefox;
static gboolean  opt_inspector;
static gboolean  opt_control;
static gboolean  opt_version;

static const GOptionEntry entries[] = {
	{ "import-firefox", 0, 0, G_OPTION_ARG_NONE, &opt_import_firefox,
	  "Replace the app's Google session with Firefox's before loading", NULL },
	{ "firefox-profile", 0, 0, G_OPTION_ARG_FILENAME, &opt_firefox_profile,
	  "Firefox profile to import from (default: the most recently used)", "DIR" },
	{ "check-firefox", 0, 0, G_OPTION_ARG_NONE, &opt_check_firefox,
	  "Report whether Firefox has a Google session to import, then exit", NULL },
	{ "inspector", 0, 0, G_OPTION_ARG_NONE, &opt_inspector,
	  "Enable the Web Inspector (F12 or Ctrl+Shift+I)", NULL },
	{ "control", 0, 0, G_OPTION_ARG_NONE, &opt_control,
	  "Open a gsurf control socket (gsurfctl -i " CONTROL_NAME ")", NULL },
	{ "version", 0, 0, G_OPTION_ARG_NONE, &opt_version,
	  "Print the version and exit", NULL },
	G_OPTION_ENTRY_NULL
};

static void wire_view(GsurfWindow *window, GsurfView *view, gpointer user_data);
static void start_import(ImportMode mode);

/* --- helpers --- */

static GtkWindow *
gtk_window_of(GsurfWindow *window)
{
	return window != NULL ? GTK_WINDOW(gsurf_window_get_native_widget(window)) : NULL;
}

static WebKitWebView *
webview_of(GsurfView *view)
{
	return WEBKIT_WEB_VIEW(gsurf_view_get_native_widget(view));
}

/* @uri without its query and fragment, for the log. */
static gchar *
uri_for_log(const gchar *uri)
{
	g_autoptr(GUri) u = uri != NULL ? g_uri_parse(uri, G_URI_FLAGS_NONE, NULL) : NULL;

	if (u == NULL)
		return g_strdup(uri != NULL ? uri : "(none)");
	return g_uri_join(G_URI_FLAGS_NONE, g_uri_get_scheme(u), NULL, g_uri_get_host(u),
		g_uri_get_port(u), g_uri_get_path(u), NULL, NULL);
}

static gboolean
close_window_idle(gpointer data)
{
	GtkWindow *native = gtk_window_of(GSURF_WINDOW(data));

	if (native != NULL)
		gtk_window_close(native);
	return G_SOURCE_REMOVE;
}

/* Close @window from the main loop, not from inside one of its own
 * view's signal handlers. */
static void
close_window_later(GsurfWindow *window)
{
	g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, close_window_idle,
		g_object_ref(window), g_object_unref);
}

/* --- dialogs --- */

/*
 * Called with the index of the chosen button, or the cancel button's
 * when the dialog is dismissed.
 */
typedef void (*AnswerFunc)(int button);

typedef struct {
	AnswerFunc done;
	int        cancel;
} Question;

static void
answer(Question *q, int button)
{
	if (q->done != NULL)
		q->done(button >= 0 ? button : q->cancel);
	g_free(q);
}

/*
 * GtkAlertDialog needs GTK 4.10. Ubuntu 22.04 has GTK 4.6, so older GTK
 * gets the GtkMessageDialog it replaced.
 */
#if GTK_CHECK_VERSION(4, 10, 0)
static void
alert_answered(GObject *source, GAsyncResult *result, gpointer user_data)
{
	g_autoptr(GError) error = NULL;

	answer(user_data, gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), result, &error));
}
#else
static void
message_answered(GtkDialog *dialog, int response, gpointer user_data)
{
	answer(user_data, response);
	gtk_window_destroy(GTK_WINDOW(dialog));
}
#endif

/* A modal dialog over the main window. @buttons is NULL-terminated. */
static void
ask(const gchar *message, const gchar *detail, const char *const *buttons,
    int cancel, int default_button, AnswerFunc done)
{
	Question *q = g_new0(Question, 1);

	q->done = done;
	q->cancel = cancel;
#if GTK_CHECK_VERSION(4, 10, 0)
	{
		g_autoptr(GtkAlertDialog) dialog = gtk_alert_dialog_new("%s", message);

		gtk_alert_dialog_set_detail(dialog, detail);
		gtk_alert_dialog_set_buttons(dialog, buttons);
		gtk_alert_dialog_set_cancel_button(dialog, cancel);
		gtk_alert_dialog_set_default_button(dialog, default_button);
		gtk_alert_dialog_choose(dialog, gtk_window_of(G.window), NULL, alert_answered, q);
	}
#else
	{
		GtkWidget *dialog = gtk_message_dialog_new(gtk_window_of(G.window),
			GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
			GTK_MESSAGE_OTHER, GTK_BUTTONS_NONE, "%s", message);

		gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s", detail);
		for (int i = 0; buttons[i] != NULL; i++)
			gtk_dialog_add_button(GTK_DIALOG(dialog), buttons[i], i);
		gtk_dialog_set_default_response(GTK_DIALOG(dialog), default_button);
		g_signal_connect(dialog, "response", G_CALLBACK(message_answered), q);
		gtk_window_present(GTK_WINDOW(dialog));
	}
#endif
}

static void
show_error(const gchar *message, const gchar *detail)
{
	static const char *const buttons[] = { "Close", NULL };

	ask(message, detail, buttons, 0, 0, NULL);
}

/* --- where links go --- */

typedef enum {
	DEST_APP,      /* load it here */
	DEST_BROWSER,  /* a web page that belongs in the system browser */
	DEST_HANDLER,  /* mailto: and tel: go to their desktop handlers */
	DEST_BLOCKED,  /* anything else: file:, intent:, ... */
} Destination;

/*
 * Google Home itself, the sign-in, consent and account pages it sends
 * you through, and the content hosts it opens as top-level pages.
 */
static gboolean
host_is_app(const gchar *host)
{
	static const gchar *const hosts[] = {
		"home.google.com",
		"accounts.google.com",
		"accounts.youtube.com",
		"myaccount.google.com",
		"consent.google.com",
		"gds.google.com",
		NULL,
	};
	g_autofree gchar *lower = NULL;

	if (host == NULL)
		return FALSE;
	lower = g_ascii_strdown(host, -1);
	return g_strv_contains(hosts, lower) || g_str_has_suffix(lower, ".googleusercontent.com");
}

static Destination
destination_for(const gchar *uri)
{
	g_autoptr(GUri) u = NULL;
	const gchar *scheme;

	/* window.open() with no URL: the page fills the popup itself. */
	if (uri == NULL || *uri == '\0')
		return DEST_APP;
	u = g_uri_parse(uri, G_URI_FLAGS_NONE, NULL);
	if (u == NULL)
		return DEST_BLOCKED;
	scheme = g_uri_get_scheme(u);
	if (g_str_equal(scheme, "https") || g_str_equal(scheme, "http"))
		return host_is_app(g_uri_get_host(u)) ? DEST_APP : DEST_BROWSER;
	if (g_str_equal(scheme, "about") || g_str_equal(scheme, "blob") || g_str_equal(scheme, "data"))
		return DEST_APP;
	if (g_str_equal(scheme, "mailto") || g_str_equal(scheme, "tel"))
		return DEST_HANDLER;
	return DEST_BLOCKED;
}

/* GtkUriLauncher needs GTK 4.10; older GTK has gtk_show_uri_full(). */
#if GTK_CHECK_VERSION(4, 10, 0)
static void
launched(GObject *source, GAsyncResult *result, gpointer user_data)
{
	g_autoptr(GError) error = NULL;

	if (!gtk_uri_launcher_launch_finish(GTK_URI_LAUNCHER(source), result, &error))
		ghd_log("cannot open link: %s", error->message);
}

static void
open_external(GsurfWindow *window, const gchar *uri)
{
	g_autoptr(GtkUriLauncher) launcher = gtk_uri_launcher_new(uri);
	g_autofree gchar *shown = uri_for_log(uri);

	ghd_log("opening outside the app: %s", shown);
	gtk_uri_launcher_launch(launcher, gtk_window_of(window), NULL, launched, NULL);
}
#else
static void
launched(GObject *source, GAsyncResult *result, gpointer user_data)
{
	g_autoptr(GError) error = NULL;

	if (!gtk_show_uri_full_finish(GTK_WINDOW(source), result, &error))
		ghd_log("cannot open link: %s", error->message);
}

static void
open_external(GsurfWindow *window, const gchar *uri)
{
	g_autofree gchar *shown = uri_for_log(uri);

	ghd_log("opening outside the app: %s", shown);
	gtk_show_uri_full(gtk_window_of(window), uri, GDK_CURRENT_TIME, NULL, launched, NULL);
}
#endif

/*
 * WebKit asks before every navigation, response and new window. gsurf
 * connects its own handler first; with no modules loaded it decides
 * nothing, so this one runs next.
 */
static gboolean
on_decide_policy(WebKitWebView *webview, WebKitPolicyDecision *decision,
                 WebKitPolicyDecisionType type, gpointer user_data)
{
	GsurfWindow *window = user_data;
	const gchar *uri;
	gboolean top_level;
	Destination dest;

	switch (type) {
	case WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION:
	case WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION: {
		WebKitNavigationAction *action = webkit_navigation_policy_decision_get_navigation_action(
			WEBKIT_NAVIGATION_POLICY_DECISION(decision));

		uri = webkit_uri_request_get_uri(webkit_navigation_action_get_request(action));
		/*
		 * A new window is always top level. A navigation action may
		 * be for an iframe, and WebKit does not say which frame it is
		 * for, so the host check for those waits for the main-frame
		 * response below. Google pages embed iframes from many hosts.
		 */
		top_level = type == WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION;
		break;
	}
	case WEBKIT_POLICY_DECISION_TYPE_RESPONSE: {
		WebKitResponsePolicyDecision *response = WEBKIT_RESPONSE_POLICY_DECISION(decision);

		/* Only top-level pages WebKit would display; downloads pass. */
		if (!webkit_response_policy_decision_is_main_frame_main_resource(response) ||
		    !webkit_response_policy_decision_is_mime_type_supported(response))
			return FALSE;
		uri = webkit_uri_response_get_uri(webkit_response_policy_decision_get_response(response));
		top_level = TRUE;
		break;
	}
	default:
		return FALSE;
	}

	dest = destination_for(uri);
	if (dest == DEST_APP || (dest == DEST_BROWSER && !top_level))
		return FALSE;

	webkit_policy_decision_ignore(decision);
	if (dest == DEST_BLOCKED) {
		g_autofree gchar *shown = uri_for_log(uri);

		ghd_log("blocked navigation to %s", shown);
	} else {
		open_external(window, uri);
	}

	/* A popup whose first page went to the browser would stay empty. */
	if (window != G.window) {
		GsurfView *view = gsurf_window_get_active_view(window);

		if (view != NULL && g_object_get_data(G_OBJECT(view), "ghd-committed") == NULL)
			close_window_later(window);
	}
	return TRUE;
}

/* --- popups --- */

static void
on_popup_ready(WebKitWebView *webview, gpointer user_data)
{
	gsurf_window_present(GSURF_WINDOW(user_data));
}

static void
on_popup_close(WebKitWebView *webview, gpointer user_data)
{
	close_window_later(GSURF_WINDOW(user_data));
}

/*
 * A popup (target=_blank, window.open) for one of the app's own hosts,
 * such as a Google sign-in or account window: a new window holding a
 * view related to @opener, so it shares the session and window.opener
 * works. WebKit loads it and asks for it to be shown with
 * "ready-to-show".
 */
static GsurfView *
open_popup(GsurfView *opener)
{
	GsurfConfig *config = gsurf_config_get_default();
	GsurfWindow *window;
	GsurfView *view;
	WebKitWebView *webview;

	view = gsurf_view_new_full(opener, NULL, NULL, TRUE);
	if (view == NULL)
		return NULL;
	gsurf_view_apply_settings(view, config->settings);

	window = gsurf_window_new();
	gsurf_window_set_default_size(window, 600, 720);
	gsurf_window_set_title(window, GHD_APP_NAME);
	g_signal_connect(window, "view-added", G_CALLBACK(wire_view), NULL);
	gsurf_window_add_view(window, view);
	gsurf_application_add_window(G.app, window);

	webview = webview_of(view);
	g_signal_connect_object(webview, "ready-to-show", G_CALLBACK(on_popup_ready), window, 0);
	g_signal_connect_object(webview, "close", G_CALLBACK(on_popup_close), window, 0);

	/* The window owns the view and the application owns the window. */
	g_object_unref(view);
	g_object_unref(window);
	return view;
}

static GsurfView *
on_create_view(GsurfView *source, const gchar *uri, gpointer user_data)
{
	GsurfWindow *window = user_data;

	switch (destination_for(uri)) {
	case DEST_APP:
		return open_popup(source);
	case DEST_BROWSER:
	case DEST_HANDLER:
		open_external(window, uri);
		return NULL;
	case DEST_BLOCKED:
	default:
		return NULL;
	}
}

/* --- page events --- */

/*
 * Google's sign-in form, its account chooser, and /CookieMismatch ("We
 * have detected a problem with your cookie settings"), where a stale or
 * conflicting session lands and a fresh import helps.
 */
static gboolean
is_signed_out_page(const gchar *uri)
{
	g_autoptr(GUri) u = uri != NULL ? g_uri_parse(uri, G_URI_FLAGS_NONE, NULL) : NULL;
	g_autofree gchar *path = NULL;

	if (u == NULL || g_strcmp0(g_uri_get_host(u), "accounts.google.com") != 0)
		return FALSE;
	path = g_ascii_strdown(g_uri_get_path(u), -1);
	return strstr(path, "signin") != NULL || strstr(path, "accountchooser") != NULL ||
	       strstr(path, "cookiemismatch") != NULL;
}

static void
signed_out_answered(int button)
{
	if (button == 1)
		start_import(IMPORT_PROMPTED);
}

/* Offered once per launch, and again after each successful import. */
static void
prompt_signed_out(void)
{
	static const char *const buttons[] = { "Sign In Here", "Import from Firefox", NULL };

	if (G.offered_import)
		return;
	G.offered_import = TRUE;
	ghd_log("signed out: offering a Firefox import");

	ask("You are not signed in to Google",
		"Sign in on this page. If Google refuses to sign you in here, sign in to "
		"home.google.com in Firefox, then import that session.",
		buttons, 0, 1, signed_out_answered);
}

static void
on_load_changed(GsurfView *view, GsurfLoadEvent event, gpointer user_data)
{
	const gchar *uri;
	g_autofree gchar *shown = NULL;

	if (event != GSURF_LOAD_COMMITTED)
		return;
	g_object_set_data(G_OBJECT(view), "ghd-committed", GINT_TO_POINTER(1));

	uri = gsurf_view_get_uri(view);
	shown = uri_for_log(uri);
	ghd_log("page: %s", shown);
	if (view == G.view && is_signed_out_page(uri))
		prompt_signed_out();
}

static gboolean
on_load_failed(WebKitWebView *webview, WebKitLoadEvent event, const gchar *uri,
               GError *error, gpointer user_data)
{
	g_autofree gchar *shown = NULL;

	/* Cancelled by on_decide_policy() or by a newer navigation. */
	if (g_error_matches(error, WEBKIT_NETWORK_ERROR, WEBKIT_NETWORK_ERROR_CANCELLED) ||
	    g_error_matches(error, WEBKIT_POLICY_ERROR,
	                    WEBKIT_POLICY_ERROR_FRAME_LOAD_INTERRUPTED_BY_POLICY_CHANGE))
		return FALSE;
	shown = uri_for_log(uri);
	ghd_log("load failed: %s: %s", shown, error->message);
	return FALSE;
}

static void
on_web_process_terminated(GsurfView *view, gpointer user_data)
{
	gint64 now = g_get_monotonic_time();

	ghd_log("web process terminated");
	/* Reload a crashed page, but not into a crash loop. */
	if (view == G.view && now - G.last_crash_reload > CRASH_RELOAD_INTERVAL) {
		G.last_crash_reload = now;
		gsurf_view_reload(view, FALSE);
	}
}

static void
on_title_changed(GsurfView *view, const gchar *title, gpointer user_data)
{
	gsurf_window_set_title(GSURF_WINDOW(user_data),
		title != NULL && *title != '\0' ? title : GHD_APP_NAME);
}

/* Every view of every window: the main one and each popup. */
static void
wire_view(GsurfWindow *window, GsurfView *view, gpointer user_data)
{
	WebKitWebView *webview = webview_of(view);

	if (g_object_get_data(G_OBJECT(view), "ghd-wired") != NULL)
		return;
	g_object_set_data(G_OBJECT(view), "ghd-wired", GINT_TO_POINTER(1));

	g_signal_connect_object(webview, "decide-policy", G_CALLBACK(on_decide_policy), window, 0);
	g_signal_connect(webview, "load-failed", G_CALLBACK(on_load_failed), NULL);
	g_signal_connect_object(view, "create-view", G_CALLBACK(on_create_view), window, 0);
	g_signal_connect_object(view, "title-changed", G_CALLBACK(on_title_changed), window, 0);
	g_signal_connect(view, "load-changed", G_CALLBACK(on_load_changed), NULL);
	g_signal_connect(view, "web-process-terminated", G_CALLBACK(on_web_process_terminated), NULL);
}

/* --- session --- */

static WebKitCookieManager *
cookie_manager(void)
{
	return webkit_network_session_get_cookie_manager(
		webkit_web_view_get_network_session(webview_of(G.view)));
}

static void
import_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
	ImportMode mode = GPOINTER_TO_INT(user_data);
	g_autoptr(GError) error = NULL;
	guint n = 0;
	gboolean ok;

	ok = ghd_firefox_import_finish(WEBKIT_COOKIE_MANAGER(source), result, &n, &error);
	if (G.view == NULL)
		return;

	if (ok) {
		ghd_log("imported %u Google cookies from Firefox", n);
		/* If that session turns out stale too, offer again. */
		G.offered_import = FALSE;
	} else {
		ghd_log("Firefox import failed: %s", error->message);
		if (mode != IMPORT_STARTUP)
			show_error("Could not import the Firefox session", error->message);
	}
	/* At startup nothing has loaded yet; after the dialog, a failure
	 * leaves the sign-in page as it is. */
	if (ok || mode != IMPORT_PROMPTED)
		gsurf_view_load_uri(G.view, GHD_HOME_URL);
}

static void
start_import(ImportMode mode)
{
	if (G.view == NULL)
		return;
	ghd_firefox_import_async(cookie_manager(), opt_firefox_profile, NULL,
		import_done, GINT_TO_POINTER(mode));
}

static void
got_startup_cookies(GObject *source, GAsyncResult *result, gpointer user_data)
{
	g_autoptr(GError) error = NULL;
	GList *cookies;
	gboolean signed_in = FALSE;

	cookies = webkit_cookie_manager_get_cookies_finish(WEBKIT_COOKIE_MANAGER(source), result, &error);
	for (GList *l = cookies; l != NULL; l = l->next)
		if (g_strcmp0(soup_cookie_get_name(l->data), "__Secure-1PSID") == 0)
			signed_in = TRUE;
	g_list_free_full(cookies, (GDestroyNotify)soup_cookie_free);
	if (error != NULL)
		ghd_log("cannot read the cookie store: %s", error->message);
	if (G.view == NULL)
		return;

	if (opt_import_firefox) {
		start_import(IMPORT_REQUESTED);
	} else if (!signed_in) {
		ghd_log("no Google session; trying to import one from Firefox");
		start_import(IMPORT_STARTUP);
	} else {
		gsurf_view_load_uri(G.view, GHD_HOME_URL);
	}
}

/* Load the app, importing a Firefox session first when there is none. */
static void
start_session(void)
{
	webkit_cookie_manager_get_cookies(cookie_manager(), "https://home.google.com/", NULL,
		got_startup_cookies, NULL);
}

/* --- window --- */

static void
save_state(void)
{
	GtkWindow *native = gtk_window_of(G.window);

	if (native == NULL)
		return;
	/* A fullscreen size is not one to come back to. */
	if (!gtk_window_is_fullscreen(native)) {
		/* GTK4 keeps the unmaximized size here while maximized. */
		gtk_window_get_default_size(native, &G.state.width, &G.state.height);
		G.state.maximized = gtk_window_is_maximized(native);
	}
	if (G.view != NULL)
		G.state.zoom = gsurf_view_get_zoom_level(G.view);
	ghd_window_state_save(&G.state);
}

static void
quit(void)
{
	save_state();
	gsurf_application_quit(G.app);
}

/*
 * Closing the main window quits, popups included: they belong to it,
 * and one WebKit never showed would otherwise keep the process alive.
 */
static gboolean
on_main_close_request(GsurfWindow *window, gpointer user_data)
{
	quit();
	/* The application drops the window once this returns. */
	G.view = NULL;
	G.window = NULL;
	return FALSE;
}

static void
zoom(GsurfView *view, gdouble factor)
{
	gdouble level = factor > 0 ? gsurf_view_get_zoom_level(view) * factor : 1.0;

	gsurf_view_set_zoom_level(view, CLAMP(level, 0.3, 3.0));
}

static gboolean
key_is(guint keyval, guint mods, const gchar *const *keys)
{
	for (; *keys != NULL; keys++)
		if (gsurf_keys_match(keyval, mods, *keys))
			return TRUE;
	return FALSE;
}

#define KEYS(...) ((const gchar *const[]){ __VA_ARGS__, NULL })

/* Browser-style shortcuts (the Electron build had none). */
static gboolean
on_key_press(GsurfWindow *window, guint keyval, guint keycode, guint mods, gpointer user_data)
{
	GsurfView *view = gsurf_window_get_active_view(window);

	if (view == NULL)
		return FALSE;

	if (key_is(keyval, mods, KEYS("Ctrl+r", "F5")))
		gsurf_view_reload(view, FALSE);
	else if (key_is(keyval, mods, KEYS("Ctrl+Shift+r", "Shift+F5")))
		gsurf_view_reload(view, TRUE);
	else if (key_is(keyval, mods, KEYS("Alt+Left", "Back")))
		gsurf_view_go_back(view);
	else if (key_is(keyval, mods, KEYS("Alt+Right", "Forward")))
		gsurf_view_go_forward(view);
	else if (key_is(keyval, mods, KEYS("Ctrl+plus", "Ctrl+Shift+plus", "Ctrl+equal", "Ctrl+KP_Add")))
		zoom(view, ZOOM_STEP);
	else if (key_is(keyval, mods, KEYS("Ctrl+minus", "Ctrl+KP_Subtract")))
		zoom(view, 1.0 / ZOOM_STEP);
	else if (key_is(keyval, mods, KEYS("Ctrl+0", "Ctrl+KP_0")))
		zoom(view, 0);
	else if (key_is(keyval, mods, KEYS("F11")))
		gsurf_window_set_fullscreen(window, !gsurf_window_get_fullscreen(window));
	else if (key_is(keyval, mods, KEYS("Ctrl+q")))
		quit();
	else if (opt_inspector && key_is(keyval, mods, KEYS("F12", "Ctrl+Shift+i")))
		webkit_web_inspector_show(webkit_web_view_get_inspector(webview_of(view)));
	else
		return FALSE;
	return TRUE;
}

/* Mouse buttons 8 and 9 (thumb buttons) go back and forward. */
static gboolean
on_button_press(GsurfWindow *window, guint button, guint state, gpointer user_data)
{
	GsurfView *view = gsurf_window_get_active_view(window);

	if (view == NULL || (button != 8 && button != 9))
		return FALSE;
	if (button == 8)
		gsurf_view_go_back(view);
	else
		gsurf_view_go_forward(view);
	return TRUE;
}

static gboolean
on_quit_signal(gpointer user_data)
{
	quit();
	return G_SOURCE_CONTINUE;
}

/* --- startup --- */

/*
 * gsurf's defaults suit a privacy-minded general browser. A single
 * trusted site wants WebGL and popups (sign-in and account windows).
 * A YAML file in the gsurf format can override any of it. On Google's
 * sites WebKit's site-specific quirks pick the user agent, so a custom
 * user_agent there also needs webkit.site_quirks off.
 */
static GsurfConfig *
build_config(void)
{
	GsurfConfig *config = gsurf_config_new();
	GsurfSettings *s = config->settings;
	g_autofree gchar *yaml = g_build_filename(g_get_user_config_dir(), GHD_APP_ID,
		"config.yaml", NULL);

	g_free(config->window_title);
	config->window_title = g_strdup(GHD_APP_NAME);
	g_free(config->cookie_jar);
	config->cookie_jar = g_build_filename(g_get_user_data_dir(), GHD_APP_ID,
		"cookies.sqlite", NULL);

	s->webgl = TRUE;
	s->js_can_open_windows = TRUE;
	/*
	 * Camera live view uses WebRTC. Fedora's WebKitGTK is built without
	 * it, which makes this a no-op there; a WebKitGTK that has it gets
	 * it.
	 */
	s->webrtc = TRUE;

	if (g_file_test(yaml, G_FILE_TEST_EXISTS)) {
		g_autoptr(GError) error = NULL;

		if (gsurf_config_load_from_file(config, yaml, &error))
			ghd_log("loaded %s", yaml);
		else
			ghd_log("ignoring %s: %s", yaml, error->message);
	}
	/* The command line wins over the file. */
	if (opt_inspector)
		s->developer_extras = TRUE;
	return config;
}

static void
start_control(void)
{
	g_autoptr(GError) error = NULL;

	G.control = gsurf_control_server_new(G.app);
	gsurf_control_server_set_name(G.control, CONTROL_NAME);
	if (!gsurf_control_server_start(G.control, NULL, &error)) {
		ghd_log("control socket: %s", error->message);
		g_clear_object(&G.control);
		return;
	}
	ghd_log("control socket open: gsurfctl -i %s", CONTROL_NAME);
}

static int
check_firefox(void)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *profile = NULL;
	GList *cookies;

	cookies = ghd_firefox_read_google_cookies(opt_firefox_profile, &profile, &error);
	if (cookies == NULL) {
		g_printerr("%s\n", error->message);
		return 1;
	}
	g_print("Firefox profile: %s\n", profile);
	g_print("Signed-in Google session with %u cookies: ready to import.\n",
		g_list_length(cookies));
	g_list_free_full(cookies, (GDestroyNotify)soup_cookie_free);
	return 0;
}

int
main(int argc, char **argv)
{
	g_autoptr(GOptionContext) context = NULL;
	g_autoptr(GError) error = NULL;
	GsurfConfig *config;
	GsurfModuleManager *modules;
	GsurfView *view;

	context = g_option_context_new(NULL);
	g_option_context_set_summary(context,
		"Google Home (home.google.com) in its own window, on gsurf and WebKitGTK.");
	g_option_context_add_main_entries(context, entries, NULL);
	if (!g_option_context_parse(context, &argc, &argv, &error)) {
		g_printerr("google-home: %s\n", error->message);
		return 2;
	}
	/* After parsing, so --help says "google-home"; before anything
	 * that reads it (GTK, WebKit, the state directory). */
	g_set_prgname(GHD_APP_ID);
	g_set_application_name(GHD_APP_NAME);
	if (opt_version) {
		g_print("google-home %s (gsurf %s)\n", GHD_VERSION, gsurf_get_version_string());
		return 0;
	}

	ghd_log_init();
	if (opt_check_firefox)
		return check_firefox();

	/* Keep WebKit's relative writes out of the launch directory. */
	if (!gsurf_runtime_dir_enter(&error)) {
		ghd_log("staying in the launch directory: %s", error->message);
		g_clear_error(&error);
	}
	gsurf_init(&argc, &argv);
	if (!gsurf_backend_init(&argc, &argv, &error)) {
		g_printerr("google-home: %s\n", error->message);
		return 1;
	}
	ghd_log("google-home %s starting: gsurf %s, WebKitGTK %u.%u.%u, %s session",
		GHD_VERSION, gsurf_get_version_string(), webkit_get_major_version(),
		webkit_get_minor_version(), webkit_get_micro_version(),
		g_getenv("XDG_SESSION_TYPE") != NULL ? g_getenv("XDG_SESSION_TYPE") : "unknown");

	config = build_config();
	gsurf_config_set_default(config);
	G.app = gsurf_application_new(config);

	/* No modules are loaded, but views dispatch their hooks through the
	 * manager, so it needs the application and config. */
	modules = gsurf_module_manager_get_default();
	gsurf_module_manager_set_application(modules, G.app);
	gsurf_module_manager_set_config(modules, config);

	ghd_window_state_load(&G.state);
	G.window = gsurf_window_new();
	gsurf_window_set_default_size(G.window, G.state.width, G.state.height);
	gsurf_window_set_title(G.window, GHD_APP_NAME);
	g_signal_connect(G.window, "view-added", G_CALLBACK(wire_view), NULL);
	g_signal_connect(G.window, "key-press", G_CALLBACK(on_key_press), NULL);
	g_signal_connect(G.window, "button-press", G_CALLBACK(on_button_press), NULL);
	/* Before the application's own handler, which drops the window. */
	g_signal_connect(G.window, "close-request", G_CALLBACK(on_main_close_request), NULL);

	view = gsurf_view_new_full(NULL, NULL, GHD_HOME_URL, FALSE);
	gsurf_view_apply_settings(view, config->settings);
	gsurf_window_add_view(G.window, view);
	gsurf_view_set_zoom_level(view, G.state.zoom);
	G.view = view;
	gsurf_application_add_window(G.app, G.window);

	if (opt_control)
		start_control();

	if (G.state.maximized)
		gtk_window_maximize(gtk_window_of(G.window));
	gsurf_window_present(G.window);
	start_session();

	/* The application owns the window, the window the view. */
	g_object_unref(view);
	g_object_unref(G.window);
	g_object_unref(config);

	g_unix_signal_add(SIGINT, on_quit_signal, NULL);
	g_unix_signal_add(SIGTERM, on_quit_signal, NULL);
	g_unix_signal_add(SIGHUP, on_quit_signal, NULL);
	gsurf_application_run(G.app);

	if (G.control != NULL) {
		gsurf_control_server_stop(G.control);
		g_object_unref(G.control);
	}
	g_object_unref(G.app);
	return 0;
}
