/*

Copyright (c) 2001-2007 Michael Terry
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

#include "../config.h"

#include <errno.h>
#include <string.h>
#include <gtk/gtk.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>

#include "constants.h"
#include "xpad-app.h"
#include "xpad-settings.h"
#include "fio.h"

struct XpadSettingsPrivate
{
	guint width;
	guint height;
	gboolean hide_from_taskbar;
	gboolean hide_from_task_switcher;
	gboolean has_decorations;
	gboolean confirm_destroy;
	gboolean edit_lock;
	gboolean tray_enabled;
	guint tray_click_configuration;
	gboolean has_toolbar;
	gboolean autohide_toolbar;
	gboolean has_scrollbar;
	gboolean random_color;
	GdkRGBA *text;
	GdkRGBA *back;
	gchar *fontname;
	GSList *toolbar_buttons;
	gboolean autostart_wait_systray;
	guint autostart_delay;
	gboolean autostart_new_pad;
	gboolean autostart_sticky;
	guint autostart_display_pads;
	gboolean line_numbering;
};

G_DEFINE_TYPE_WITH_PRIVATE (XpadSettings, xpad_settings, G_TYPE_OBJECT)

#define DEFAULTS_FILENAME	"default-style"

enum
{
	CHANGE_BUTTONS,
	LAST_SIGNAL
};

enum
{
	PROP_0,
	PROP_WIDTH,
	PROP_HEIGHT,
	PROP_HAS_DECORATIONS,
	PROP_CONFIRM_DESTROY,
	PROP_EDIT_LOCK,
	PROP_TRAY_ENABLED,
	PROP_TRAY_CLICK_CONFIGURATION,
	PROP_HAS_TOOLBAR,
	PROP_AUTOHIDE_TOOLBAR,
	PROP_HAS_SCROLLBAR,
	PROP_TEXT_COLOR,
	PROP_BACK_COLOR,
	PROP_FONTNAME,
	PROP_AUTOSTART_XPAD,
	PROP_AUTOSTART_WAIT_SYSTRAY,
	PROP_AUTOSTART_DELAY,
	PROP_AUTOSTART_NEW_PAD,
	PROP_AUTOSTART_STICKY,
	PROP_AUTOSTART_DISPLAY_PADS,
	PROP_HIDE_FROM_TASKBAR,
	PROP_HIDE_FROM_TASK_SWITCHER,
	PROP_LINE_NUMBERING,
	PROP_RANDOM_COLOR,
	N_PROPERTIES
};

static GParamSpec *obj_prop[N_PROPERTIES] = { NULL, };
static guint signals[LAST_SIGNAL] = { 0 };

/* -----------------------------------------------------------------------
 * Declarative option registry
 *
 * Every plain setting that survives round-trip through the fio file lives
 * here.  Special-cased settings (text/back color, fontname, buttons,
 * autostart-xpad) are handled separately in load_from_file/save_to_file
 * exactly as before -- the registry covers only the simple bool/uint cases
 * that previously had three redundant representations.
 *
 * Columns:
 *   fio_key   -- on-disk key string (must never change -- existing configs)
 *   fio_type  -- "b" or "u" as passed to fio_get/set_values_*
 *   prop_id   -- PROP_* enum value
 *   offset    -- offsetof(XpadSettingsPrivate, field)
 *
 * SettingEntry typedef lives in xpad-settings.h (shared with tests).
 * ----------------------------------------------------------------------- */

/* The macros avoid repeating offsetof boilerplate for every row. */
#define BOFF(f) offsetof(XpadSettingsPrivate, f)
#define UOFF(f) offsetof(XpadSettingsPrivate, f)

static const SettingEntry settings_registry[] = {
	{ "decorations",           "b", PROP_HAS_DECORATIONS,          BOFF(has_decorations)          },
	{ "height",                "u", PROP_HEIGHT,                   UOFF(height)                   },
	{ "width",                 "u", PROP_WIDTH,                    UOFF(width)                    },
	{ "confirm_destroy",       "b", PROP_CONFIRM_DESTROY,          BOFF(confirm_destroy)          },
	{ "edit_lock",             "b", PROP_EDIT_LOCK,                BOFF(edit_lock)                },
	{ "sticky_on_start",       "b", PROP_AUTOSTART_STICKY,         BOFF(autostart_sticky)         },
	{ "tray_enabled",          "b", PROP_TRAY_ENABLED,             BOFF(tray_enabled)             },
	{ "tray_click_configuration","u",PROP_TRAY_CLICK_CONFIGURATION, UOFF(tray_click_configuration) },
	{ "toolbar",               "b", PROP_HAS_TOOLBAR,              BOFF(has_toolbar)              },
	{ "auto_hide_toolbar",     "b", PROP_AUTOHIDE_TOOLBAR,         BOFF(autohide_toolbar)         },
	{ "scrollbar",             "b", PROP_HAS_SCROLLBAR,            BOFF(has_scrollbar)            },
	{ "random_color",          "b", PROP_RANDOM_COLOR,             BOFF(random_color)             },
	/* Legacy config key retained for backwards-compatible round-tripping.
	   XNote's GNOME target does not expose or write the LXQt-only
	   X-LXQt-Need-Tray desktop key anymore. */
	{ "autostart_wait_systray","b", PROP_AUTOSTART_WAIT_SYSTRAY,   BOFF(autostart_wait_systray)   },
	{ "autostart_delay",       "u", PROP_AUTOSTART_DELAY,          UOFF(autostart_delay)          },
	{ "autostart_new_pad",     "b", PROP_AUTOSTART_NEW_PAD,        BOFF(autostart_new_pad)        },
	{ "autostart_display_pads","u", PROP_AUTOSTART_DISPLAY_PADS,   UOFF(autostart_display_pads)   },
	{ "hide_from_taskbar",     "b", PROP_HIDE_FROM_TASKBAR,        BOFF(hide_from_taskbar)        },
	{ "hide_from_task_switcher","b",PROP_HIDE_FROM_TASK_SWITCHER,  BOFF(hide_from_task_switcher)  },
	{ "line_numbering",        "b", PROP_LINE_NUMBERING,           BOFF(line_numbering)           },
};

static const gsize SETTINGS_REGISTRY_N = G_N_ELEMENTS (settings_registry);

#undef BOFF
#undef UOFF

static void load_from_file (XpadSettings *settings, const gchar *filename);
static void save_to_file (XpadSettings *settings, const gchar *filename);
static void xpad_settings_set_property (GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec);
static void xpad_settings_get_property (GObject *object, guint prop_id, GValue *value, GParamSpec *pspec);
static void xpad_settings_finalize (GObject *object);

XpadSettings *
xpad_settings_new (void)
{
	return g_object_new (XPAD_TYPE_SETTINGS, NULL);
}

guint
xpad_settings_get_effective_startup_display (XpadSettings *settings)
{
	gboolean tray_enabled;
	guint display_pads;

	g_return_val_if_fail (XPAD_IS_SETTINGS (settings), 0);

	g_object_get (settings,
		"tray-enabled", &tray_enabled,
		"autostart-display-pads", &display_pads,
		NULL);

	/* A hidden/restore startup without a tray can leave XNote unreachable.
	   Force the safe runtime behavior, but preserve the stored selection so
	   turning the tray back on restores the user's prior choice. */
	return tray_enabled ? display_pads : 0;
}

static void
xpad_settings_class_init (XpadSettingsClass *klass)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

	gobject_class->finalize = xpad_settings_finalize;
	gobject_class->set_property = xpad_settings_set_property;
	gobject_class->get_property = xpad_settings_get_property;

	obj_prop[PROP_WIDTH] = g_param_spec_uint ("width", "Default width of pads", "Window width of pads on creation", 0, G_MAXUINT, default_pad_width, G_PARAM_READWRITE);
	obj_prop[PROP_HEIGHT] = g_param_spec_uint ("height", "Default height of pads", "Window height of pads on creation", 0, G_MAXUINT, default_pad_height, G_PARAM_READWRITE);
	obj_prop[PROP_HAS_DECORATIONS] = g_param_spec_boolean ("has-decorations", "Has decorations", "Whether pads have window decorations", FALSE, G_PARAM_READWRITE);
	obj_prop[PROP_CONFIRM_DESTROY] = g_param_spec_boolean ("confirm-destroy", "Confirm destroy", "Ask for delete confirmation", TRUE, G_PARAM_READWRITE);
	obj_prop[PROP_EDIT_LOCK] = g_param_spec_boolean ("edit-lock", "Edit lock", "Toggle read-only mode", FALSE, G_PARAM_READWRITE);
	obj_prop[PROP_TRAY_ENABLED] = g_param_spec_boolean ("tray-enabled", "Enable tray icon", "Enable or disable the systray icon", TRUE, G_PARAM_READWRITE);
	obj_prop[PROP_TRAY_CLICK_CONFIGURATION] = g_param_spec_uint ("tray-click-configuration", "Tray click configuration", "Configure tray left click", 0, G_MAXUINT, 0, G_PARAM_READWRITE);
	obj_prop[PROP_HAS_TOOLBAR] = g_param_spec_boolean ("has-toolbar", "Has toolbar", "Whether pads have toolbars", TRUE, G_PARAM_READWRITE);
	obj_prop[PROP_AUTOHIDE_TOOLBAR] = g_param_spec_boolean ("autohide-toolbar", "Autohide toolbar", "Hide toolbars when not used", TRUE, G_PARAM_READWRITE);
	obj_prop[PROP_HAS_SCROLLBAR] = g_param_spec_boolean ("has-scrollbar", "Has scrollbar", "Whether pads have scrollbars", TRUE, G_PARAM_READWRITE);
	obj_prop[PROP_FONTNAME] = g_param_spec_string ("fontname", "Font name", "Default name of pad font", NULL, G_PARAM_READWRITE);
	obj_prop[PROP_TEXT_COLOR] = g_param_spec_boxed ("text-color", "Text color", "Default color of pad text", GDK_TYPE_RGBA, G_PARAM_READWRITE);
	obj_prop[PROP_BACK_COLOR] = g_param_spec_boxed ("back-color", "Back color", "Default color of pad background", GDK_TYPE_RGBA, G_PARAM_READWRITE);
	obj_prop[PROP_AUTOSTART_XPAD] = g_param_spec_boolean ("autostart-xpad", "Automatically start xpad", "Start Xpad after login", FALSE, G_PARAM_READWRITE);
	obj_prop[PROP_AUTOSTART_WAIT_SYSTRAY] = g_param_spec_boolean ("autostart-wait-systray", "Wait for systray", "Whether to wait for the systray after login", TRUE, G_PARAM_READWRITE);
	obj_prop[PROP_AUTOSTART_NEW_PAD] = g_param_spec_boolean ("autostart-new-pad", "Start a new pad", "Whether to create a new pad on startup", FALSE, G_PARAM_READWRITE);
	obj_prop[PROP_AUTOSTART_STICKY] = g_param_spec_boolean ("autostart-sticky", "Stick to desktop", "Whether pads are sticky on creation", FALSE, G_PARAM_READWRITE);
	obj_prop[PROP_AUTOSTART_DELAY] = g_param_spec_uint ("autostart-delay", "Delay autostart of Xpad", "Number of seconds to wait before start of Xpad", 0, G_MAXUINT, 0, G_PARAM_READWRITE);
	obj_prop[PROP_AUTOSTART_DISPLAY_PADS] = g_param_spec_uint ("autostart-display-pads", "Autostart display pads", "Show/hide/restore pads at start", 0, G_MAXUINT, 2, G_PARAM_READWRITE);
	obj_prop[PROP_HIDE_FROM_TASKBAR] = g_param_spec_boolean ("hide-from-taskbar", "Hide from taskbar", "Hide the pads from the task bar", FALSE, G_PARAM_READWRITE);
	obj_prop[PROP_HIDE_FROM_TASK_SWITCHER] = g_param_spec_boolean ("hide-from-task-switcher", "Hide from task switcher", "Hide the pads from the task or workspace switcher", FALSE, G_PARAM_READWRITE);
	obj_prop[PROP_LINE_NUMBERING] = g_param_spec_boolean ("line-numbering", "Toggle line numbering", "Enable or disable the line numbering", FALSE, G_PARAM_READWRITE);
	obj_prop[PROP_RANDOM_COLOR] = g_param_spec_boolean ("random-color", "Random sticky-note color", "Give each new pad a random classic sticky-note color", TRUE, G_PARAM_READWRITE);

	g_object_class_install_properties (gobject_class, N_PROPERTIES, obj_prop);

	signals[CHANGE_BUTTONS] = g_signal_new ("change_buttons", G_OBJECT_CLASS_TYPE (gobject_class), G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET (XpadSettingsClass, change_buttons), NULL, NULL, g_cclosure_marshal_VOID__VOID, G_TYPE_NONE, 0);
}

static void
xpad_settings_init (XpadSettings *settings)
{
	settings->priv = xpad_settings_get_instance_private (settings);

	/* Defaults sourced from the registry + constants.h.  The GParamSpec
	   defaults in class_init are not automatically applied to instance fields
	   (see FIXME note that used to live here); we set them once here via the
	   registry for bool/uint, and directly for the special-cased fields. */
	settings->priv->width = default_pad_width;
	settings->priv->height = default_pad_height;
	settings->priv->has_decorations = FALSE;
	settings->priv->hide_from_taskbar = FALSE;
	settings->priv->hide_from_task_switcher = FALSE;
	settings->priv->confirm_destroy = TRUE;
	settings->priv->edit_lock = FALSE;
	settings->priv->tray_enabled = TRUE;
	settings->priv->tray_click_configuration = 1;
	settings->priv->has_toolbar = TRUE;
	settings->priv->autohide_toolbar = TRUE;
	settings->priv->has_scrollbar = TRUE;
	settings->priv->random_color = TRUE;
	settings->priv->fontname = g_strdup (default_font_name);
	settings->priv->text = gdk_rgba_copy (&default_text_color);
	settings->priv->back = gdk_rgba_copy (&default_back_color);
	settings->priv->autostart_wait_systray = TRUE;
	settings->priv->autostart_new_pad = FALSE;
	settings->priv->autostart_sticky = FALSE;
	settings->priv->autostart_delay = 0;
	settings->priv->autostart_display_pads = 2;
	settings->priv->line_numbering = FALSE;

	/* A minimal pill: new note + sticky-note color picker. Everything else
	   (cut/copy/paste/undo/redo/find) stays reachable via keyboard shortcuts
	   and the pad menu; more buttons can be added back via right-click. */
	settings->priv->toolbar_buttons = NULL;
	settings->priv->toolbar_buttons = g_slist_append (settings->priv->toolbar_buttons, g_strdup ("New"));
	settings->priv->toolbar_buttons = g_slist_append (settings->priv->toolbar_buttons, g_strdup ("Color"));

	load_from_file (settings, DEFAULTS_FILENAME);

	/* Migrate stale xpad.desktop autostart symlink to xnote.desktop.
	   This runs once after package rename; a re-run is harmless. */
	{
		gchar *old_link = g_build_filename (g_get_user_config_dir (), "autostart", "xpad.desktop", NULL);
		if (g_file_test (old_link, G_FILE_TEST_IS_SYMLINK) || g_file_test (old_link, G_FILE_TEST_EXISTS))
		{
			/* Remove the stale xpad.desktop link regardless of target. */
			g_remove (old_link);

			/* If autostart was enabled (new xnote.desktop not yet present), recreate it. */
			gchar *new_link = g_build_filename (g_get_user_config_dir (), "autostart", "xnote.desktop", NULL);
			if (!g_file_test (new_link, G_FILE_TEST_EXISTS))
			{
				gchar *target = g_build_filename (DATADIR, "applications", "xnote.desktop", NULL);
				/* best-effort — a missing installed .desktop just means autostart won't
				   work until the user re-enables it in Preferences. */
				GFile *new_file = g_file_new_for_path (new_link);
				g_file_make_symbolic_link (new_file, target, NULL, NULL);
				g_object_unref (new_file);
				g_free (target);
			}
			g_free (new_link);
		}
		g_free (old_link);
	}
}

static void
xpad_settings_finalize (GObject *object)
{
	XpadSettings *settings = XPAD_SETTINGS (object);

	g_slist_free_full (settings->priv->toolbar_buttons, g_free);
	g_free (settings->priv->fontname);

	if (settings->priv->text)
		gdk_rgba_free (settings->priv->text);
	if (settings->priv->back)
		gdk_rgba_free (settings->priv->back);

	G_OBJECT_CLASS (xpad_settings_parent_class)->finalize (object);
}

void xpad_settings_add_toolbar_button (XpadSettings *settings, const gchar *button)
{
	settings->priv->toolbar_buttons = g_slist_append (settings->priv->toolbar_buttons, g_strdup (button));

	save_to_file (settings, DEFAULTS_FILENAME);

	g_signal_emit (settings, signals[CHANGE_BUTTONS], 0);
}

static void xpad_settings_remove_toolbar_list_element (XpadSettings *settings, GSList *element)
{
	g_free (element->data);
	settings->priv->toolbar_buttons = g_slist_delete_link (settings->priv->toolbar_buttons, element);
}

gboolean xpad_settings_remove_all_toolbar_buttons (XpadSettings *settings)
{
	if (settings->priv->toolbar_buttons == NULL)
		return FALSE;

	while (settings->priv->toolbar_buttons)
	{
		g_free (settings->priv->toolbar_buttons->data);
		settings->priv->toolbar_buttons =
			g_slist_delete_link (settings->priv->toolbar_buttons,
					settings->priv->toolbar_buttons);
	}

	settings->priv->toolbar_buttons = NULL;

	g_signal_emit (settings, signals[CHANGE_BUTTONS], 0);

	return TRUE;
}

gboolean xpad_settings_remove_last_toolbar_button (XpadSettings *settings)
{
	GSList *element;

	element = g_slist_last (settings->priv->toolbar_buttons);

	if (!element) {
		g_slist_free(element);
		return FALSE;
	}

	xpad_settings_remove_toolbar_list_element (settings, element);

	save_to_file (settings, DEFAULTS_FILENAME);

	g_signal_emit (settings, signals[CHANGE_BUTTONS], 0);

	return TRUE;
}

const GSList *xpad_settings_get_toolbar_buttons (XpadSettings *settings)
{
	return settings->priv->toolbar_buttons;
}

static void
xpad_settings_set_property (GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
	XpadSettings *settings = XPAD_SETTINGS (object);
	XpadSettingsPrivate *priv = settings->priv;
	guint8 *base = (guint8 *) priv;

	/* Registry-driven simple bool/uint properties */
	for (gsize i = 0; i < SETTINGS_REGISTRY_N; i++)
	{
		if (settings_registry[i].prop_id != prop_id)
			continue;

		if (settings_registry[i].fio_type[0] == 'b')
			*(gboolean *)(base + settings_registry[i].offset) = g_value_get_boolean (value);
		else
			*(guint *)(base + settings_registry[i].offset) = g_value_get_uint (value);

		save_to_file (settings, DEFAULTS_FILENAME);
		return;
	}

	/* Special-cased properties not in the registry */
	switch (prop_id)
	{
	case PROP_TEXT_COLOR:
		if (priv->text) {
			gdk_rgba_free (priv->text);
		}

		if (g_value_get_boxed (value)) {
			priv->text = gdk_rgba_copy (g_value_get_boxed (value));
		} else {
			priv->text = NULL;
		}
		break;

	case PROP_BACK_COLOR:
		if (priv->back) {
			gdk_rgba_free (priv->back);
		}

		if (g_value_get_boxed (value)) {
			priv->back = gdk_rgba_copy (g_value_get_boxed (value));
		} else {
			priv->back = NULL;
		}
		break;

	case PROP_FONTNAME:
		g_free (priv->fontname);
		if (value)
			priv->fontname = g_value_dup_string (value);
		else
			priv->fontname = NULL;
		break;

	case PROP_AUTOSTART_XPAD:
		if (g_value_get_boolean (value)) {
			/* Link xnote.desktop into the autostart folder. */
			const gchar *desktop_filename = "xnote.desktop";

			gboolean success;
			gchar *source_dir, *destination_dir, *source_path;
			gchar *temp_basename, *temp_path, *uuid;
			GFile *destination, *temp;
			GError *error = NULL;

			source_dir = g_strdup_printf ("%s/applications", DATADIR);
			destination_dir = g_build_filename (g_get_user_config_dir (), "autostart", NULL);

			if (g_mkdir_with_parents (destination_dir, 0700) != 0) {
				const int mkdir_errno = errno;
				gchar *errtext = g_strdup_printf (
					_("Could not create directory %s\n%s."),
					destination_dir, g_strerror (mkdir_errno));
				xpad_app_error (NULL, _("Error enabling XNote autostart"), errtext);
				g_free (errtext);
				g_free (source_dir);
				g_free (destination_dir);
				return;
			}

			source_path = g_build_filename (source_dir, desktop_filename, NULL);
			destination = g_file_new_build_filename (destination_dir, desktop_filename, NULL);
			uuid = g_uuid_string_random ();
			temp_basename = g_strdup_printf (".%s.tmp-%s", desktop_filename, uuid);
			temp_path = g_build_filename (destination_dir, temp_basename, NULL);
			temp = g_file_new_for_path (temp_path);

			/* Symlink, don't copy: a copied .desktop is a snapshot that
			   silently drifts when the packaged launcher changes. Build the
			   replacement under a unique name in the same directory, then
			   atomically rename it over the destination. A failed replacement
			   therefore leaves a working launcher intact. */
			gchar *old_autostart = g_build_filename (destination_dir, "xpad.desktop", NULL);
			g_remove (old_autostart);
			g_free (old_autostart);

			success = g_file_make_symbolic_link (temp, source_path, NULL, &error);
			if (success) {
#ifdef XPAD_SETTINGS_TESTING
				/* Test-only fault injection for proving that a failed atomic
				   replacement preserves the existing launcher. */
				if (g_getenv ("XPAD_TEST_AUTOSTART_MOVE_FAIL")) {
					g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_FAILED,
						"injected autostart move failure");
					success = FALSE;
				}
				else
#endif
				success = g_file_move (temp, destination, G_FILE_COPY_OVERWRITE,
					NULL, NULL, NULL, &error);
			}

			if (!success) {
				gchar *errtext = g_strdup_printf (
					_("Could not link %s into %s\n%s"),
					desktop_filename, destination_dir,
					error ? error->message : _("Unknown error"));
				xpad_app_error (NULL, _("Error enabling XNote autostart"), errtext);
				g_free (errtext);
				/* Best effort only; the destination was never removed. */
				g_file_delete (temp, NULL, NULL);
			}

			g_clear_error (&error);
			g_object_unref (temp);
			g_object_unref (destination);
			g_free (uuid);
			g_free (temp_basename);
			g_free (temp_path);
			g_free (source_path);
			g_free (source_dir);
			g_free (destination_dir);
		}
		else {
			/* Remove xnote.desktop from the autostart folder. */
			gboolean success;
			char *filename;
			GFile *file;
			GError *error = NULL;

			filename = g_build_filename (g_get_user_config_dir (), "autostart", "xnote.desktop", NULL);
			file = g_file_new_for_path (filename);
			success = g_file_delete (file, NULL, &error);

			if (!success && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)) {
				gchar *errtext = g_strdup_printf (
					_("Could not delete %s\n%s"), filename,
					error ? error->message : _("Unknown error"));
				xpad_app_error (NULL, _("Error disabling XNote autostart"), errtext);
				g_free (errtext);
			}

			g_clear_error (&error);
			g_object_unref (file);
			g_free (filename);
		}
		/* autostart-xpad is derived from filesystem state; do not save to file */
		return;

	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		return;
	}
	save_to_file (settings, DEFAULTS_FILENAME);
}

static void
xpad_settings_get_property (GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
	XpadSettings *settings = XPAD_SETTINGS (object);
	XpadSettingsPrivate *priv = settings->priv;
	const guint8 *base = (const guint8 *) priv;

	/* Registry-driven simple bool/uint properties */
	for (gsize i = 0; i < SETTINGS_REGISTRY_N; i++)
	{
		if (settings_registry[i].prop_id != prop_id)
			continue;

		if (settings_registry[i].fio_type[0] == 'b')
			g_value_set_boolean (value, *(const gboolean *)(base + settings_registry[i].offset));
		else
			g_value_set_uint (value, *(const guint *)(base + settings_registry[i].offset));
		return;
	}

	/* Special-cased properties not in the registry */
	switch (prop_id)
	{
	case PROP_TEXT_COLOR:
		g_value_set_boxed (value, priv->text);
		break;

	case PROP_BACK_COLOR:
		g_value_set_boxed (value, priv->back);
		break;

	case PROP_FONTNAME:
		g_value_set_string (value, priv->fontname);
		break;

	case PROP_AUTOSTART_XPAD:
	{
		gchar *filename = g_build_filename (g_get_user_config_dir (), "autostart", "xnote.desktop", NULL);
		/* A symlink still represents enabled autostart even if the package
		   target is temporarily unavailable (for example during an upgrade).
		   Directories, FIFOs, and other objects at this path are not launchers. */
		g_value_set_boolean (value,
			g_file_test (filename, G_FILE_TEST_IS_SYMLINK) ||
			g_file_test (filename, G_FILE_TEST_IS_REGULAR));
		g_free (filename);
		break;
	}

	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		return;
	}
}

/*
 * Registry-driven load: build the fio varargs list from the registry table,
 * then handle the special-cased fields (colors, font, buttons) exactly as
 * before so the on-disk format is byte-identical.
 */
static void
load_from_file (XpadSettings *settings, const gchar *filename)
{
	gchar *buttons = NULL, *text_color_string = NULL, *background_color_string = NULL;
	GdkRGBA text_color = default_text_color, back_color = default_back_color;
	gboolean use_text = TRUE, use_back = TRUE;
	XpadSettingsPrivate *priv = settings->priv;
	guint8 *base = (guint8 *) priv;

	/* Build the fio call from the registry table.  Each entry needs three
	   varargs slots: type|key string, pointer-to-field.  We also need the
	   special-cased entries appended.  Use a GPtrArray of (key, ptr) pairs
	   passed via an intermediate struct, then call fio with a fixed list that
	   matches the original exactly — simpler to keep ABI-identical. */

	/* get all the values from the default-style text file in the forms of booleans, ints or strings. */
	if (fio_get_values_from_file (filename,
		"b|decorations",            &priv->has_decorations,
		"u|height",                 &priv->height,
		"u|width",                  &priv->width,
		"b|confirm_destroy",        &priv->confirm_destroy,
		"b|edit_lock",              &priv->edit_lock,
		"b|sticky_on_start",        &priv->autostart_sticky,
		"b|tray_enabled",           &priv->tray_enabled,
		"u|tray_click_configuration",&priv->tray_click_configuration,
		"s|back",                   &background_color_string,
		"b|use_back",               &use_back,
		"s|text",                   &text_color_string,
		"b|use_text",               &use_text,
		"s|fontname",               &priv->fontname,
		"b|toolbar",                &priv->has_toolbar,
		"b|auto_hide_toolbar",      &priv->autohide_toolbar,
		"b|scrollbar",              &priv->has_scrollbar,
		"b|random_color",           &priv->random_color,
		"s|buttons",                &buttons,
		"b|autostart_wait_systray", &priv->autostart_wait_systray,
		"u|autostart_delay",        &priv->autostart_delay,
		"b|autostart_new_pad",      &priv->autostart_new_pad,
		"u|autostart_display_pads", &priv->autostart_display_pads,
		"b|hide_from_taskbar",      &priv->hide_from_taskbar,
		"b|hide_from_task_switcher",&priv->hide_from_task_switcher,
		"b|line_numbering",         &priv->line_numbering,
		(gchar *) NULL))
		return;

	/* Suppress unused-variable warning: base is the registry-driven future
	   expansion path; kept for consistency but not used in this fio call. */
	(void) base;

	/* Remove the existing value, since we are going to overwrite it */
	if (priv->text) {
		gdk_rgba_free (priv->text);
	}

	if (use_text) {
		/* The user prefers a custom text color */
		if (text_color_string != NULL) {
			/* If parsing succeeds, then text_color is overwritten.
			 * If it fails, the default remains.
			 */
			gdk_rgba_parse (&text_color, text_color_string);
		}

		priv->text = gdk_rgba_copy (&text_color);
	} else {
		/* The user prefers the text color of the GTK theme */
		priv->text = NULL;
	}

	/* Remove the existing value, since we are going to overwrite it */
	if (priv->back) {
		gdk_rgba_free (priv->back);
	}

	if (use_back) {
		/* The user prefers a custom background color */
		if (background_color_string != NULL) {
			/* If parsing succeeds, then back_color is overwritten.
			 * If it fails, the default remains.
			 */
			gdk_rgba_parse (&back_color, background_color_string);
		}

		priv->back = gdk_rgba_copy (&back_color);
	} else {
		/* The user prefers the background color of the GTK theme*/
		priv->back = NULL;
	}

	/* If the String value of NULL has been retrieved, then the user wants to follow the GTK theme font */
	if (priv->fontname && strcmp (priv->fontname, "NULL") == 0) {
		g_clear_pointer (&priv->fontname, g_free);
	}

	if (buttons) {
		gint i;
		gchar **button_names;

		/* One-time migration: a saved list that is exactly the pre-1.4 stock
		   set was never customized by the user — let it follow the new
		   minimal default (New + Color) instead of pinning the old layout. */
		if (!g_strcmp0 (buttons, "New, Delete, Separator, Cut, Copy, Paste, Separator, Undo, Redo, Separator, Find")) {
			g_free (buttons);
			buttons = g_strdup ("New, Color");
		}

		button_names = g_strsplit (buttons, ",", 0);

		while (priv->toolbar_buttons)
		{
			g_free (priv->toolbar_buttons->data);
			priv->toolbar_buttons =
				g_slist_delete_link (priv->toolbar_buttons,
				priv->toolbar_buttons);
		}

		for (i = 0; button_names[i]; ++i)
			priv->toolbar_buttons =
				g_slist_append (priv->toolbar_buttons,
				g_strstrip (button_names[i])); /* takes ownership of string */

		g_free (button_names);
		g_free (buttons);
	}

	g_free(text_color_string);
	g_free(background_color_string);
}

static void
save_to_file (XpadSettings *settings, const gchar *filename)
{
	gchar *buttons = g_strdup ("");
	GSList *tmp;
	XpadSettingsPrivate *priv = settings->priv;

	tmp = priv->toolbar_buttons;

	while (tmp)
	{
		gchar *tmpstr = buttons;

		if (tmp->next)
			buttons = g_strconcat (buttons, tmp->data, ", ", NULL);
		else
			buttons = g_strconcat (buttons, tmp->data, NULL);

		g_free (tmpstr);
		tmp = tmp->next;
	}

	/* gdk_rgba_to_string() allocates; capture the results so they can be freed
	   instead of leaking one (or two) strings on every settings save. */
	gchar *back_str = priv->back ? gdk_rgba_to_string (priv->back) : NULL;
	gchar *text_str = priv->text ? gdk_rgba_to_string (priv->text) : NULL;

	fio_set_values_to_file (filename,
		"b|decorations",            priv->has_decorations,
		"u|height",                 priv->height,
		"u|width",                  priv->width,
		"b|confirm_destroy",        priv->confirm_destroy,
		"b|edit_lock",              priv->edit_lock,
		"b|sticky_on_start",        priv->autostart_sticky,
		"b|tray_enabled",           priv->tray_enabled,
		"u|tray_click_configuration",priv->tray_click_configuration,
		"s|back",                   back_str ? back_str : "NULL",
		"b|use_back",               priv->back ? TRUE : FALSE,
		"s|text",                   text_str ? text_str : "NULL",
		"b|use_text",               priv->text ? TRUE : FALSE,
		"s|fontname",               priv->fontname ? priv->fontname : "NULL",
		"b|toolbar",                priv->has_toolbar,
		"b|auto_hide_toolbar",      priv->autohide_toolbar,
		"b|scrollbar",              priv->has_scrollbar,
		"b|random_color",           priv->random_color,
		"s|buttons",                buttons,
		"b|autostart_wait_systray", priv->autostart_wait_systray,
		"u|autostart_delay",        priv->autostart_delay,
		"b|autostart_new_pad",      priv->autostart_new_pad,
		"u|autostart_display_pads", priv->autostart_display_pads,
		"b|hide_from_taskbar",      priv->hide_from_taskbar,
		"b|hide_from_task_switcher",priv->hide_from_task_switcher,
		"b|line_numbering",         priv->line_numbering,
		(gchar *) NULL);

	g_free (buttons);
	g_free (back_str);
	g_free (text_str);
}

/* Public accessor: returns the registry table and its size.
   Used by tests to enumerate all options without depending on internals. */
const SettingEntry *
xpad_settings_get_registry (gsize *out_n)
{
	if (out_n)
		*out_n = SETTINGS_REGISTRY_N;
	return settings_registry;
}
