/*

Copyright (c) 2001-2007 Michael Terry
Copyright (c) 2009 Paul Ivanov
Copyright (c) 2013-2024 Arthur Borsboom

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

*/

/* required by socket stuff */
/* define _GNU_SOURCE here because that makes our sockets work nice
 Unfortunately, we lose portability... */

#include "../config.h"

#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <glib.h>
#include <glib-unix.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <adwaita.h>

#include "xpad-app.h"
#include "fio.h"
#include "help.h"
#include "xpad-backup.h"
#include "xpad-pad.h"
#include "xpad-pad-group.h"
#include "xpad-periodic.h"
#include "xpad-tray.h"

/* Seems that some systems (sun-sparc-solaris2.8 at least), need the following three #defines.
   These were provided by Alan Mizrahi.
*/
#ifndef PF_LOCAL
#define PF_LOCAL PF_UNIX
#endif

#ifndef AF_LOCAL
#define AF_LOCAL AF_UNIX
#endif

#ifndef SUN_LEN
#define SUN_LEN(sunp) ((size_t)((struct sockaddr_un *)0)->sun_path + strlen((sunp)->sun_path))
#endif

static gint xpad_argc;
static gchar **xpad_argv;
static gboolean option_nonew;
static gboolean option_new;
static gboolean option_hide;
static gboolean option_show;
static gboolean option_toggle;
/* Accepted and ignored: XSMP is gone with X11 (3.0.0), but an X session
   manager may still hold a saved restart command from an older XNote that
   passes it. Parsing it as a no-op keeps that launch working; rejecting it
   made XNote exit(1) and never start. */
static gchar *option_smid_ignored;
static gboolean option_version;
static gboolean option_quit;
static gboolean shutdown_in_progress;
static gchar **option_files;
static gchar *config_dir;
static gchar *program_path;
static gchar *server_filename;
static gint server_fd;
static FILE *output;
static XpadPadGroup *pad_group;
static gint pads_loaded_on_start = 0;
static XpadSettings *settings;
/* GTK 4 removed gtk_main()/gtk_main_quit()/gtk_main_level(); xpad is not a
   GtkApplication (it runs its own socket-based single-instance), so we drive
   our own GLib main loop instead. */
static GMainLoop *main_loop = NULL;

static gboolean		process_local_args          (gint *argc, gchar **argv[]);
static gboolean		process_remote_args         (gint *argc, gchar **argv[], gboolean have_gtk, XpadSettings *xpad_settings);

static gboolean		config_dir_exists           (void);
static gchar		*make_config_dir            (void);
static void		register_stock_icons        (void);
static gint		xpad_app_load_pads          (void);
static gboolean		xpad_app_first_idle_check   (XpadPadGroup *group);
static gboolean		xpad_app_pass_args          (void);
static gboolean		xpad_app_open_proc_file     (void);
static void enable_unix_signal_handlers();
static gboolean on_unix_signal(gpointer data);

static void
xpad_app_init (int argc, char **argv)
{
	gboolean first_time;
	gboolean have_gtk;
	shutdown_in_progress = FALSE;

	/* Set up support different languages */
#ifdef ENABLE_NLS
	gchar *locale_dir = g_strdup_printf ("%s/%s", DATADIR, LOCALEDIR);
	bindtextdomain (GETTEXT_PACKAGE, locale_dir);
	g_free(locale_dir);
	bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
	textdomain (GETTEXT_PACKAGE);
#endif

	/* GTK 4 gtk_init_check() takes no arguments and no longer strips GTK
	   options from argv; xpad does its own option parsing below. */
	have_gtk = gtk_init_check ();
	xpad_argc = argc;
	xpad_argv = argv;
	output = stdout;

	/* Bring libadwaita up before touching the config dir, so the migration
	   error dialogs in make_config_dir() have a valid Adw context (guarded on
	   have_gtk; the headless --version/remote path below never needs it). */
	if (have_gtk)
		adw_init ();

	/* Set up config directory. */
	first_time = !config_dir_exists ();
	config_dir = make_config_dir ();

	/* create master socket name */
	server_filename = g_build_filename (xpad_app_get_config_dir (), "server", NULL);

	if (!have_gtk)
	{
		/* We don't have GTK+, but we can still do
		   --version or --help and such.  Plus, we
		   can pass commands to a remote instance. */
		process_local_args (&xpad_argc, &xpad_argv);
		if (!xpad_app_pass_args ())
		{
			process_remote_args (&xpad_argc, &xpad_argv, FALSE, settings);
			fprintf (output, "%s\n", _("XNote is a graphical program.  Please run it from your desktop."));
		}
		exit (0);
	}

	/* libadwaita was initialised above (before the config-dir migration). */

	/* App-level CSS: shape the hover toolbar into a floating pill on top of
	   the theme's .osd.toolbar look, and give its buttons a tighter round
	   hit-target. Keyed on the XpadToolbar class so themes can override. */
	{
		GtkCssProvider *css = gtk_css_provider_new ();
		gtk_css_provider_load_from_data (css,
			".XpadToolbar { padding: 3px 6px; border-radius: 16px; border-spacing: 2px; }"
			".XpadToolbar button { min-width: 26px; min-height: 26px; border-radius: 13px; padding: 2px; margin: 0; }"
			".XpadToolbar separator { margin-top: 5px; margin-bottom: 5px; margin-left: 1px; margin-right: 1px; }",
			-1);
		gtk_style_context_add_provider_for_display (gdk_display_get_default (),
			GTK_STYLE_PROVIDER (css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
		g_object_unref (css);
	}

	g_set_application_name (_("XNote"));
	/* gdk_set_program_class() was removed in GTK 4; the WM class now comes
	   from the application id / .desktop file instead. */

	/* Set up program path. */
	if (xpad_argc > 0)
		program_path = g_find_program_in_path (xpad_argv[0]);
	else
		program_path = NULL;

	process_local_args (&xpad_argc, &xpad_argv);

	if (xpad_app_pass_args ())
		exit (0);

	/* Race condition here, between calls */
	xpad_app_open_proc_file ();

	register_stock_icons ();
	/* gtk_window_set_default_icon_name() was removed in GTK 4; each pad
	   window sets its own icon name, and the app id supplies the rest. */

	/* Read the Xpad configuration file from disk (if exists) */
	settings = xpad_settings_new ();

	/* Delay program startup, if user configured it, to wait for example for the loading of the systray. */
	guint autostart_delay;
	g_object_get (settings, "autostart-delay", &autostart_delay, NULL);

	if (autostart_delay)
		sleep(autostart_delay);

	pad_group = xpad_pad_group_new();
	process_remote_args (&xpad_argc, &xpad_argv, TRUE, settings);

	/* load all pads */
	pads_loaded_on_start = xpad_app_load_pads ();

	if (pads_loaded_on_start == 0 && !option_new) {
		if (!option_nonew) {
			GtkWidget *pad = xpad_pad_new (pad_group, settings);

			/* Only show a new pad on startup, if the generic setting says to show all pads on startup. */
			guint display_pads =
				xpad_settings_get_effective_startup_display (settings);

			if (display_pads == 0) {
				gtk_widget_show (pad);
			}
		}
	}

    /* Load the optional app indicator / tray */
	xpad_tray_init (settings);

	/* Initialize Xpad-periodic module */
	xpad_periodic_init ();
	xpad_periodic_set_callback ("save-content", (XpadPeriodicFunc) xpad_pad_save_content);
	xpad_periodic_set_callback ("save-info", (XpadPeriodicFunc) xpad_pad_save_info);

	/* Cloud note backup: saves above also schedule a debounced run of the
	   xpad-cloud-backup helper. */
	xpad_backup_init ();

	g_idle_add ((GSourceFunc)xpad_app_first_idle_check, pad_group);

	if (first_time) {
		g_object_set (settings, "autostart-xpad", TRUE, NULL);
		show_help ();
	}

	g_free (server_filename);
	server_filename = NULL;
}

gint main (gint argc, gchar **argv)
{
	/* No backend or renderer pin. XNote follows the session like any other
	   GTK 4 app.

	   Both pins existed only to prop up X11. GDK_BACKEND=x11 was forced
	   because pad positioning and the WM hints were raw Xlib; GSK_RENDERER=
	   cairo was forced because NVIDIA + *XWayland* GL surfaces silently drop
	   their contents after a KVM/DDC switch or DPMS cycle. Removing X11
	   removes the cause of both, and the XWayland round-trip that could hang
	   the main loop mid-snapshot goes with it.

	   XPAD_GDK_BACKEND / XPAD_GSK_RENDERER are kept as overrides, but they
	   are now opt-in rather than a default: CI uses them to force a backend
	   in a headless compositor, and they are the escape hatch if a specific
	   GPU ever needs a different renderer. */
	const gchar *backend = g_getenv ("XPAD_GDK_BACKEND");
	const gchar *renderer = g_getenv ("XPAD_GSK_RENDERER");
	if (backend && *backend)
		g_setenv ("GDK_BACKEND", backend, TRUE);
	if (renderer && *renderer)
		g_setenv ("GSK_RENDERER", renderer, TRUE);

	xpad_app_init (argc, argv);
	enable_unix_signal_handlers();

	main_loop = g_main_loop_new (NULL, FALSE);
	g_main_loop_run (main_loop);
	g_clear_pointer (&main_loop, g_main_loop_unref);

	return 0;
}

/* Replacement for gtk_main_level() > 0: is our main loop actually running? */
gboolean
xpad_app_is_running (void)
{
	return main_loop != NULL && g_main_loop_is_running (main_loop);
}

/* Replacement for gtk_main_quit(): stop the GLib main loop without tearing
   down any windows. Used by the session manager too. */
void
xpad_app_main_quit (void)
{
	if (main_loop != NULL && g_main_loop_is_running (main_loop))
		g_main_loop_quit (main_loop);
}

/* SIGINT/SIGTERM are delivered through the GLib main loop, so the full
   save-and-teardown in xpad_app_quit() runs in normal main context. The old
   raw signal() handler called GTK/GIO from async-signal context, where a
   signal landing mid-save could deadlock or corrupt the very save it
   triggered. (SIGQUIT keeps its default core-dump action on purpose.) */
static void enable_unix_signal_handlers() {
	g_unix_signal_add(SIGINT, on_unix_signal, NULL);
	g_unix_signal_add(SIGTERM, on_unix_signal, NULL);
}

static gboolean on_unix_signal(gpointer data)
{
	(void) data;

	xpad_app_quit();

	return G_SOURCE_REMOVE;
}

/* parent and secondary may be NULL.
 * Returns when user dismisses error.
 */
void
xpad_app_error (GtkWindow *parent, const gchar *primary, const gchar *secondary)
{
	GtkWidget *dialog;

	g_printerr ("%s\n", primary);

	dialog = xpad_app_alert_dialog (parent, "dialog-error", primary, secondary);
	adw_message_dialog_add_response (ADW_MESSAGE_DIALOG (dialog), "ok", _("_Ok"));
	adw_message_dialog_set_default_response (ADW_MESSAGE_DIALOG (dialog), "ok");
	xpad_app_alert_dialog_run (dialog);
}

const gchar *
xpad_app_get_config_dir (void)
{
	return config_dir;
}

/* Returns absolute path to our own executable. May be NULL. */
const gchar *
xpad_app_get_program_path (void)
{
	return program_path;
}

XpadPadGroup *
xpad_app_get_pad_group (void)
{
	return pad_group;
}

void
xpad_app_quit (void)
{
	if (shutdown_in_progress) {
		return;
	}

	shutdown_in_progress = TRUE;

	/* Stop the main loop. This does no destruction of windows; it just exits
	   the loop and returns to the caller in main(). */
	xpad_app_main_quit ();

	/* First disable the signals, then free the memory used by the tray icon and its menu. */
	xpad_tray_dispose (settings);

	/* First disable the accelerators, then free the memory used by the pads belonging to this group */
	xpad_pad_group_destroy_pads (pad_group);

	/* Pads saved any pending changes while being destroyed; if a backup was
	   scheduled, launch it now — the helper process outlives xpad. */
	xpad_backup_flush ();

	/* Free the memory used by group. */
	g_clear_object (&pad_group);

	/* Free the memory used by the settings menu. */
	g_clear_object (&settings);

	/* A clean quit leaves no socket behind: the backup helper's restore only
	   dials it, but a stale file still misleads anyone looking at the dir. */
	g_unlink (server_filename);

	exit(EXIT_SUCCESS);
}

static gboolean
config_dir_exists (void)
{
	gchar *dir = NULL;
	gboolean exists = FALSE;

	/* New location: ~/.config/xnote (PACKAGE = "xnote") */
	dir = g_build_filename (g_get_user_config_dir (), PACKAGE, NULL);
	exists = g_file_test (dir, G_FILE_TEST_EXISTS);
	g_free (dir);

	if (!exists)
	{
		/* Legacy xnote location: ~/.xnote */
		dir = g_build_filename (g_get_home_dir (), "." PACKAGE, NULL);
		exists = g_file_test (dir, G_FILE_TEST_EXISTS);
		g_free (dir);
	}

	if (!exists)
	{
		/* Old xpad location: ~/.config/xpad — will be migrated in make_config_dir.
		   Probe explicitly with a literal so existing-user first_time stays FALSE. */
		dir = g_build_filename (g_get_user_config_dir (), "xpad", NULL);
		exists = g_file_test (dir, G_FILE_TEST_EXISTS);
		g_free (dir);
	}

	if (!exists)
	{
		/* Old legacy xpad location: ~/.xpad */
		dir = g_build_filename (g_get_home_dir (), ".xpad", NULL);
		exists = g_file_test (dir, G_FILE_TEST_EXISTS);
		g_free (dir);
	}

	return exists;
}

static void
make_path (const gchar *path)
{
	GSList *dirs = NULL, *i;
	gchar *dirname;

	dirname = g_strdup (path);

	while (!g_file_test (dirname, G_FILE_TEST_EXISTS))
	{
		dirs = g_slist_prepend (dirs, dirname);
		dirname = g_path_get_dirname (dirname);
	}
	g_free (dirname);

	for (i = dirs; i; i = i->next)
	{
		g_mkdir ((gchar *) i->data, 0700);
		g_free (i->data);
	}

	g_slist_free(dirs);
	g_slist_free(i);
}

/* Recursively copy a directory tree, skipping the named socket file.
   Used as a cross-filesystem fallback when g_rename() fails. */
static gboolean
copy_dir_recursive (const gchar *src, const gchar *dst, const gchar *skip_name)
{
	GDir *dir;
	const gchar *name;
	gboolean ok = TRUE;

	if (g_mkdir (dst, 0700) != 0 && !g_file_test (dst, G_FILE_TEST_IS_DIR))
		return FALSE;

	dir = g_dir_open (src, 0, NULL);
	if (!dir)
		return FALSE;

	while ((name = g_dir_read_name (dir)) != NULL)
	{
		gchar *s = g_build_filename (src, name, NULL);
		gchar *d = g_build_filename (dst, name, NULL);

		if (skip_name && g_strcmp0 (name, skip_name) == 0)
		{
			/* skip the live server socket */
		}
		else if (g_file_test (s, G_FILE_TEST_IS_DIR))
		{
			ok = copy_dir_recursive (s, d, skip_name) && ok;
		}
		else
		{
			gchar *contents = NULL;
			gsize len = 0;
			if (g_file_get_contents (s, &contents, &len, NULL))
				ok = g_file_set_contents (d, contents, (gssize)len, NULL) && ok;
			else
				ok = FALSE;
			g_free (contents);
		}

		g_free (s);
		g_free (d);
	}
	g_dir_close (dir);
	return ok;
}

/* Best-effort recursive delete, used to clean up a partially-copied
   destination so the next launch can retry the migration from the intact
   source rather than starting with half the notes. */
static void
remove_dir_recursive (const gchar *path)
{
	GDir *dir = g_dir_open (path, 0, NULL);
	const gchar *name;

	if (dir)
	{
		while ((name = g_dir_read_name (dir)) != NULL)
		{
			gchar *child = g_build_filename (path, name, NULL);
			if (g_file_test (child, G_FILE_TEST_IS_DIR))
				remove_dir_recursive (child);
			else
				g_unlink (child);
			g_free (child);
		}
		g_dir_close (dir);
	}
	g_rmdir (path);
}

/* Show a fatal migration error and exit. Uses an AdwMessageDialog when a
   display is present (adw_init() has already run for that case), otherwise
   falls back to stderr so a headless launch cannot crash on uninitialized
   libadwaita. Never returns. */
static void
migration_fatal (const gchar *primary, const gchar *secondary)
{
	if (gdk_display_get_default () != NULL)
	{
		GtkWidget *dialog = xpad_app_alert_dialog (NULL, NULL, primary, secondary);
		adw_message_dialog_add_response (ADW_MESSAGE_DIALOG (dialog), "quit", _("Quit"));
		xpad_app_alert_dialog_run (dialog);
	}
	else
		g_printerr ("%s: %s\n", primary, secondary);
	exit (1);
}

/**
 * Creates the directory if it does not exist.
 * Returns newly allocated dir name, NULL if an error occurred.
 */
/* Returns TRUE only if a process is actually listening on the given AF_UNIX
   socket path. A crashed/killed xpad leaves its "server" socket file on disk (it
   is not unlinked on exit), so an existence check is not proof of a live
   instance — connect() is. */
static gboolean
unix_socket_is_live (const gchar *path)
{
	struct sockaddr_un addr;
	int fd;
	gboolean live;

	fd = socket (PF_LOCAL, SOCK_STREAM, 0);
	if (fd < 0)
		return FALSE;

	bzero (&addr, sizeof (addr));
	addr.sun_family = AF_LOCAL;
	strncpy (addr.sun_path, path, sizeof (addr.sun_path) - 1);

	/* cppcheck-suppress nullPointer ; SUN_LEN expands offsetof, i.e. a literal (struct sockaddr_un *) 0 */
	live = (connect (fd, (struct sockaddr *) &addr, SUN_LEN (&addr)) == 0);
	close (fd);
	return live;
}

static gchar *
make_config_dir (void)
{
	gchar *dir = NULL;

	make_path (g_get_user_config_dir ());

	/* New config dir: ~/.config/xnote (PACKAGE) */
	dir = g_build_filename (g_get_user_config_dir (), PACKAGE, NULL);

	if (!g_file_test (dir, G_FILE_TEST_EXISTS))
	{
		gchar *olddir_xpad;
		gchar *olddir_legacy;

		/* --- xpad → xnote migration (explicit literals, not PACKAGE) --- */
		olddir_xpad = g_build_filename (g_get_user_config_dir (), "xpad", NULL);

		if (g_file_test (olddir_xpad, G_FILE_TEST_EXISTS))
		{
			/* Safety: refuse to migrate only if a LIVE xpad instance is running.
			   A crashed/killed xpad leaves a stale server socket file behind (it is
			   not unlinked on exit), so the old existence check blocked the migration
			   forever. Probe for an actual listener instead. A stale socket carried
			   into the new dir is harmless — xnote unlinks its own server socket on
			   startup — so we do not touch it here (never risk racing a live writer). */
			gchar *old_socket = g_build_filename (olddir_xpad, "server", NULL);
			gboolean live = g_file_test (old_socket, G_FILE_TEST_EXISTS)
			             && unix_socket_is_live (old_socket);
			g_free (old_socket);

			if (live)
			{
				/* xpad is genuinely running; bail rather than racing an active writer. */
				migration_fatal (_("XNote cannot start"),
					_("An existing xpad instance is running. Please close xpad before starting XNote so your notes can be migrated safely."));
			}

			/* Attempt atomic rename first (same filesystem). */
			if (g_rename (olddir_xpad, dir) != 0)
			{
				/* Cross-filesystem or other rename failure: copy then remove. */
				if (!copy_dir_recursive (olddir_xpad, dir, "server"))
				{
					/* Copy failed partway: delete the partial destination so the next
					   launch retries from the intact source instead of starting with
					   half the notes, then abort. */
					remove_dir_recursive (dir);
					migration_fatal (_("XNote cannot migrate notes"),
						g_strdup_printf (_("Could not move or copy %s to %s. "
						                   "Please move your notes manually and try again."),
						                 olddir_xpad, dir));
				}
				/* Copy succeeded; remove the old dir tree. */
				remove_dir_recursive (olddir_xpad);
			}
		}
		else
		{
			/* --- legacy ~/.xpad → ~/.config/xnote migration (original upstream path) --- */
			olddir_legacy = g_build_filename (g_get_home_dir (), ".xpad", NULL);

			if (g_file_test (olddir_legacy, G_FILE_TEST_EXISTS))
				g_rename (olddir_legacy, dir);
			else
				g_mkdir (dir, 0700); /* give user all rights */

			g_free (olddir_legacy);
		}

		g_free (olddir_xpad);
	}

	return dir;
}

/**
 * Creates an AdwMessageDialog with heading 'primary' and body 'secondary'.
 * No responses (buttons) are added; the caller adds them with
 * adw_message_dialog_add_response() before calling xpad_app_alert_dialog_run().
 *
 * The icon_name argument is retained for source compatibility but ignored:
 * AdwMessageDialog renders no leading icon.
 */
GtkWidget *
xpad_app_alert_dialog (GtkWindow *parent, const gchar *icon_name, const gchar *primary, const gchar *secondary)
{
	(void) icon_name;

	GtkWidget *dialog = adw_message_dialog_new (parent, primary, secondary);
	gtk_window_set_destroy_with_parent (GTK_WINDOW (dialog), TRUE);

	return dialog;
}

/* Stores the response id from the most recent xpad_app_alert_dialog_run(). */
static gchar *alert_dialog_response = NULL;

static void
alert_dialog_response_cb (AdwMessageDialog *dialog, const char *response, gpointer user_data)
{
	(void) dialog;
	GMainLoop *loop = user_data;

	g_free (alert_dialog_response);
	alert_dialog_response = g_strdup (response);

	if (g_main_loop_is_running (loop))
		g_main_loop_quit (loop);
}

/**
 * Presents an AdwMessageDialog and blocks in a nested main loop until the user
 * picks a response, mimicking the old gtk_dialog_run(). Returns the chosen
 * response id (owned by xpad-app, valid until the next call). AdwMessageDialog
 * closes and frees itself once a response is emitted.
 */
const gchar *
xpad_app_alert_dialog_run (GtkWidget *dialog)
{
	GMainLoop *loop = g_main_loop_new (NULL, FALSE);

	g_signal_connect (dialog, "response", G_CALLBACK (alert_dialog_response_cb), loop);
	gtk_window_present (GTK_WINDOW (dialog));
	g_main_loop_run (loop);
	g_main_loop_unref (loop);

	return alert_dialog_response;
}

static void
register_stock_icons (void)
{
	/* GTK 4: icon themes are per-display and gtk_icon_theme_get_default() /
	   _prepend_search_path() are gone. */
	GtkIconTheme *theme = gtk_icon_theme_get_for_display (gdk_display_get_default ());
	gchar *theme_dir = g_strdup_printf ("%s/%s", DATADIR, THEMEDIR);
	gtk_icon_theme_add_search_path (theme, theme_dir);
	g_free(theme_dir);
}

static gboolean
xpad_app_first_idle_check (XpadPadGroup *group)
{
	static guint tries = 0;

	/* We do this check at the first idle rather than immediately during
	   start because we want to give the tray time to become embedded.
	   has_indicator is only meaningful once the async bus-name and
	   watcher-presence queries resolve — until then, retry briefly instead
	   of racing them (a lost race showed all pads / quit spuriously). */
	if (!xpad_tray_ready () && tries++ < 30) {
		g_timeout_add (100, (GSourceFunc) xpad_app_first_idle_check, group);
		return FALSE;
	}

	if (!xpad_tray_has_indicator () &&
	    xpad_pad_group_num_visible_pads (group) == 0)
	{
		if (pads_loaded_on_start > 0)
			/* So we loaded xpad, there's no tray, and there's only hidden
			   pads...  Probably previously had tray open but we failed
			   this time.  Show all pads as a last resort.  This shouldn't
			   happen in normal operation. */
			xpad_pad_group_show_all (group);
		else
		{
			if (xpad_app_is_running ())
				xpad_app_quit ();
			else
				exit (0);
		}
	}

	return FALSE;
}

/* Scans config directory for pad files and loads them. */
static gint
xpad_app_load_pads (void)
{
	gint opened = 0;
	GDir *dir;
	GSList *names, *l;

	dir = g_dir_open (xpad_app_get_config_dir (), 0, NULL);

	if (!dir)
	{
		gchar *errtext;

		errtext = g_strdup_printf (_("Could not open directory %s."), xpad_app_get_config_dir ());

		xpad_app_error (NULL, errtext,
			_("This directory is needed to store preference and pad information.  XNote will close now."));
		g_free (errtext);

		exit (1);
	}

	names = fio_info_names (dir);
	g_dir_close (dir);

	for (l = names; l; l = l->next)
	{
		gboolean show = TRUE;
		GtkWidget *pad = xpad_pad_new_with_info (pad_group, settings, l->data, &show);
		/*
		 * show = refers to the hidden variable in the info file; this can be different for each pad; show = !hidden.
		 * option_show = command line parameter to show all the pads
		 * option_hide = command line parameter to hide all the pads
		*/

		if ((show || option_show) && !option_hide) {
			/* Map restored pads without gtk_window_present(): calling
			   gtk_window_present() on every pad at login sends an
			   xdg_activation_v1 focus request per note across workspaces,
			   which the compositor denies as focus-stealing and turns into
			   demands-attention urgency on the dock icon. */
			gtk_widget_set_visible (pad, TRUE);
		} else if (show) {
			/* pad thought it would show, we should save that it didn't */
			xpad_pad_save_info_delayed (XPAD_PAD (pad));
		}

		opened ++;
	}

	g_slist_free_full (names, g_free);

	return opened;
}

/*
converts main program arguments into one long string.
puts allocated string in dest, and returns size
*/
static guint
args_to_string (int argc, char **argv, char **dest)
{
	gint i = 0;
	guint size = 0;
	gchar *p = NULL;
	size_t string_length = 0;

	for (i = 0; i < argc; i++) {
		string_length = strlen (argv[i]) + 1;

		/* safe cast */
		if( string_length <= UINT_MAX ) {
		       size += (guint) string_length;
		}
		else {
			g_warning("casting the size of the arguments failed");
		}
	}

	*dest = g_malloc (size);

	p = *dest;

	for (i = 0; i < argc; i++)
	{
		strcpy (p, argv[i]);
		p += strlen (argv[i]);
		p[0] = ' ';
		p += 1;
	}

	p --;
	p[0] = '\0';

	return size;
}

/*
returns number of strings in newly allocated argv
*/
static guint
string_to_args (const char *string, char ***argv)
{
	guint num, i;
	const gchar *tmp;
	char **list;

	/* first, find out how many arguments we have */
	num = 1;
	for (tmp = strchr (string, ' '); tmp; tmp = strchr (tmp+1, ' '))
		num++;

	list = (char **) g_malloc (sizeof (char *) * (num + 1));

	for (i = 0; i < num; i++)
	{
		size_t len;

		/* string points to beginning of current arg */
		tmp = strchr (string, ' '); /* NULL or end of this arg */

		if (tmp) {
			long int difference = tmp - string;
			/* safe cast from long int to size_t */
			if (difference >= 0)
				len = (size_t) difference;
			else {
				g_warning("Error casting argument length. Arguments might not be processed correctly.");
				len = 0;
			}
		}
		else
			len = strlen (string);

		list[i] = g_malloc (len + 1);
		memcpy (list[i], string, len);
		list[i][len] = '\0';

		/* Make string point to beginning of next arg. tmp is NULL on the
		   final argument -- NULL + 1 is undefined behaviour even though the
		   loop is about to end, so only advance when there is a next arg. */
		if (tmp)
			string = tmp + 1;
	}

	list[i] = NULL;  /* null terminate list */

	*argv = list;

	return num;
}

/* This reads a line from the proc file.  This line will contain arguments to process. */
static void
xpad_app_read_from_proc_file (void)
{
	gint client_fd;
	guint size = 0;
	gint argc;
	gchar **argv;
	gchar *args;
	struct sockaddr_un client;
	socklen_t client_len;
	ssize_t bytes = -1;

	/* accept waiting connection */
	client_len = sizeof (client);
	client_fd = accept (server_fd, (struct sockaddr *) &client, &client_len);
	if (client_fd == -1)
		return;

	/* get size of args and verify for errors */
	bytes = read (client_fd, &size, sizeof (size));
	if (bytes == -1 || bytes != sizeof(size)) {
		g_warning("Cannot read proc file correctly");
		goto close_client_fd;
	}

	/* alloc memory */
	args = (gchar *) g_malloc (size);
	if (!args)
		goto close_client_fd;

	/* read args */
	bytes = read (client_fd, args, size);
	if (bytes < size)
		goto close_client_fd;

	argc = (gint) string_to_args (args, &argv);

	g_free (args);

	/* here we redirect singleton->priv->output to the socket */
	output = fdopen (client_fd, "w");

	if (!process_remote_args (&argc, &argv, TRUE, settings))
	{
		/* if there were no non-local arguments, insert --new as argument */
		gint c = 2;
		gchar **v = NULL;
		unsigned long int my_size = 0;
		/* safe cast */
		my_size = sizeof (gchar *) * (long unsigned) c;
		v = g_malloc (my_size);
		v[0] = PACKAGE;
		v[1] = "--new";

		process_remote_args (&c, &v, TRUE, settings);

		g_free (v);
	}

	/* restore standard singleton->priv->output */
	fclose (output);
	output = stdout;

	g_strfreev (argv);
	return;

close_client_fd:
	close (client_fd);
}

static gboolean
can_read_from_server_fd (GIOChannel *source, GIOCondition condition, gpointer data)
{
	/* A dirty way to silence the compiler for these unused variables. */
	(void) source;
	(void) condition;
	(void) data;

	xpad_app_read_from_proc_file ();

	return TRUE;
}

static gboolean
xpad_app_open_proc_file (void)
{
	GIOChannel *channel;
	struct sockaddr_un master;

	g_unlink (server_filename);

	/* create the socket */
	server_fd = socket (PF_LOCAL, SOCK_STREAM, 0);
	bzero (&master, sizeof (master));
	master.sun_family = AF_LOCAL;
	strcpy (master.sun_path, server_filename);

	/* cppcheck-suppress nullPointer ; SUN_LEN expands offsetof, i.e. a literal (struct sockaddr_un *) 0 */
	if (bind (server_fd, (struct sockaddr *) &master, SUN_LEN (&master)))
		return FALSE;

	/* listen for connections */
	if (listen (server_fd, 5))
		return FALSE;

	/* set up input loop, waiting for read */
	channel = g_io_channel_unix_new (server_fd);
	g_io_add_watch (channel, G_IO_IN, can_read_from_server_fd, NULL);
	g_io_channel_unref (channel);

	return TRUE;
}

static gboolean
xpad_app_pass_args (void)
{
	int client_fd;
	struct sockaddr_un master;
	fd_set fdset;
	fd_set exception;
	gchar buf [129];
	gchar *args = NULL;
	guint size;
	ssize_t bytesRead;
	gboolean connected = FALSE;
	ssize_t error;

	/* create master socket */
	client_fd = socket (PF_LOCAL, SOCK_STREAM, 0);
	master.sun_family = AF_LOCAL;
	strcpy (master.sun_path, server_filename);

	/* connect to master socket */
	/* cppcheck-suppress nullPointer ; SUN_LEN expands offsetof, i.e. a literal (struct sockaddr_un *) 0 */
	if (connect (client_fd, (struct sockaddr *) &master, SUN_LEN (&master)))
		goto done;
	connected = TRUE;

	size = args_to_string (xpad_argc, xpad_argv, &args) + 1;

	/* first, write length of string */
	error = write (client_fd, &size, sizeof (size));
	if (error == -1)
		g_error("There is a problem writing information to the socket.");

	/* now, write string */
	error = write (client_fd, args, (size_t) size);
	if (error == -1)
		g_error("There is a problem writing information to the socket.");

	do
	{
		/* wait for response */
		FD_ZERO (&fdset);
		FD_ZERO (&exception);
		FD_SET (client_fd, &fdset);
		FD_SET (client_fd, &exception);
		/* block until we are answered, or an error occurs */
		select (client_fd + 1, &fdset, NULL, &exception, NULL);

		do
		{
			bytesRead = read (client_fd, buf, 128);

			if (bytesRead < 0)
			{
			  goto done;
			}

			buf[bytesRead] = '\0';
			printf ("%s", buf);
		}
		while (bytesRead > 0);
	}
	while (bytesRead > 0);

done:
	close (client_fd);

	g_free (args);

	return connected;
}

/**
 * Here are the functions called when arguments are passed to us.
 */

static GOptionEntry local_options[] =
{
	{"version", 'v', 0, G_OPTION_ARG_NONE, &option_version, N_("Show version number and quit"), NULL},
	{"no-new", 'N', 0, G_OPTION_ARG_NONE, &option_nonew, N_("Don't create a new pad on startup if no previous pads exist"), NULL},
	{NULL}
};

static GOptionEntry remote_options[] =
{
	{"new", 'n', 0, G_OPTION_ARG_NONE, &option_new, N_("Create a new pad on startup even if pads already exist"), NULL},
	{"hide", 'h', 0, G_OPTION_ARG_NONE, &option_hide, N_("Hide all pads"), NULL},
	{"show", 's', 0, G_OPTION_ARG_NONE, &option_show, N_("Show all pads"), NULL},
	{"toggle", 't', 0, G_OPTION_ARG_NONE, &option_toggle, N_("Toggle between show and hide all pads"), NULL},
	{"new-from-file", 'f', 0, G_OPTION_ARG_FILENAME_ARRAY, &option_files, N_("Create a new pad with the contents of a file"), N_("FILE")},
	{"quit", 'q', 0, G_OPTION_ARG_NONE, &option_quit, N_("Close all pads"), NULL},
	/* Compatibility no-op -- see option_smid_ignored. Deliberately NOT part of
	   the "there are remote args" result below: it triggers no action. */
	{"sm-client-id", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_STRING, &option_smid_ignored, NULL, NULL},
	{NULL}
};

static gboolean
process_local_args (gint *argc, gchar **argv[])
{
	GError *error = NULL;
	GOptionContext *context;
	gint argc_copy;
	gchar **argv_copy;

	option_version = FALSE;
	option_nonew = FALSE;

	/* We make copies of argc and argv because we actually don't want the
	   behavior of g_option_context_parse() that removes entries from the
	   array. */
	argc_copy = *argc;
	argv_copy = g_strdupv (*argv);

	context = g_option_context_new (NULL);
	g_option_context_add_main_entries (context, local_options, GETTEXT_PACKAGE);
	/* We do remote here as well, because we want --help to pick them up.  It
	   can't hurt since they only set the global values that we reset later. */
	g_option_context_add_main_entries (context, remote_options, GETTEXT_PACKAGE);
	if (g_option_context_parse (context, &argc_copy, &argv_copy, &error))
	{
		if (option_version)
		{
			fprintf (output, _("XNote %s"), PACKAGE_VERSION);
			fprintf (output, "\n");
			exit (0);
		}
	}
	else
	{
		fprintf (output, "%s\n", error->message);
		exit (1);
	}

	g_option_context_free (context);
	g_strfreev (argv_copy);

	return (option_version || option_nonew);
}

static gboolean
process_remote_args (gint *argc, gchar **argv[], gboolean have_gtk, XpadSettings *xpad_settings)
{
	GError *error = NULL;
	GOptionContext *context;

	option_new = FALSE;
	option_files = NULL;
	option_quit = FALSE;
	g_clear_pointer (&option_smid_ignored, g_free);
	option_hide = FALSE;
	option_show = FALSE;
	option_toggle = FALSE;

	context = g_option_context_new (NULL);
	g_option_context_set_ignore_unknown_options (context, TRUE);
	g_option_context_set_help_enabled (context, FALSE);
	g_option_context_add_main_entries (context, remote_options, GETTEXT_PACKAGE);

	if (g_option_context_parse (context, argc, argv, &error)) {
		/* "Open a new empty pad" is a startup preference. This function also
		   serves every command a second xnote sends to the running instance
		   (--show, --hide, --toggle, a file), and those must not spawn a pad. */
		static gboolean startup_args_done = FALSE;
		gboolean is_startup = !startup_args_done;

		if (!option_new && !startup_args_done) {
			g_object_get (settings, "autostart-new-pad", &option_new, NULL);
		}
		startup_args_done = TRUE;

		if (have_gtk && option_new) {
			GtkWidget *pad = xpad_pad_new (pad_group, settings);
			if (is_startup)
				gtk_widget_set_visible (pad, TRUE);
			else
				gtk_window_present (GTK_WINDOW (pad));
		}

		if (have_gtk && (option_show)) {
			xpad_pad_group_show_all (pad_group);
		}

		if (have_gtk && (option_hide)) {
			xpad_pad_group_close_all (pad_group);
		}

		if (have_gtk && option_toggle) {
			xpad_pad_group_toggle_hide (pad_group);
		}

		if (!option_hide && !option_show) {
			guint display_pads =
				xpad_settings_get_effective_startup_display (xpad_settings);

			if (display_pads == 0) {
				option_show = TRUE;
			} else if (display_pads == 1) {
				option_hide = TRUE;
			}
		}

		if (have_gtk && option_files) {
			int i;

			for (i = 0; option_files[i]; i++) {
				GtkWidget *pad = xpad_pad_new_from_file (pad_group, settings, option_files[i]);

				if (pad) {
					if (is_startup)
						gtk_widget_set_visible (pad, TRUE);
					else
						gtk_window_present (GTK_WINDOW (pad));
				}
			}
		}

		if (option_quit) {
			if (have_gtk && xpad_app_is_running ()) {
				xpad_app_quit ();
			} else {
				exit (0);
			}
		}
	} else {
		fprintf (output, "%s\n", error->message);
		/* Don't quit.  Bad options passed to the main xpad program by other
		   iterations shouldn't close the main one. */
	}

	g_option_context_free (context);

	return(option_new || option_quit || option_files ||
	       option_hide || option_show || option_toggle);
}
