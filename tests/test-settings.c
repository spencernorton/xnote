/*
 * test-settings.c -- GLib unit tests for XpadSettings option registry.
 *
 * Verifies:
 *   1. Every plain bool/uint property in the registry round-trips through
 *      fio save->load with a non-default value.
 *   2. The registry has exactly the expected number of entries.
 *   3. Every expected fio key string is present in the registry (config
 *      format is unchanged).
 *
 * Links: xpad-settings.c, fio.c, constants.c, GTK (for GdkRGBA).
 * xpad_app_* is stubbed out.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <stdlib.h>
#include <string.h>

#include "xpad-settings.h"
#include "fio.h"
#include "constants.h"

static gchar *test_config_dir;

/* ---- Stubs ---- */

void
xpad_app_error (void *parent, const gchar *primary, const gchar *secondary)
{
	(void) parent;
	g_printerr ("xpad_app_error: %s\n", primary ? primary : "(null)");
}

const gchar *
xpad_app_get_config_dir (void)
{
	return test_config_dir;
}

/* ---- helpers ---- */

/* Set HOME + XDG_CONFIG_HOME to a fresh temp dir, create the xnote subdir
   and make it the config dir fio saves default-style in, return the temp
   root (caller must free). */
static gchar *
setup_temp_home (void)
{
	gchar *tmpl = g_strdup ("/tmp/test-settings-XXXXXX");
	gchar *dir = g_mkdtemp (tmpl);
	g_assert_nonnull (dir);

	test_config_dir = g_build_filename (dir, ".config", "xnote", NULL);
	g_assert_cmpint (g_mkdir_with_parents (test_config_dir, 0700), ==, 0);

	g_setenv ("HOME", dir, TRUE);
	g_setenv ("XDG_CONFIG_HOME", g_build_filename (dir, ".config", NULL), TRUE);

	return tmpl; /* tmpl is already the actual dir path after mkdtemp */
}

static void
remove_temp_home (const gchar *dir)
{
	/* Remove default-style file then the dir tree. */
	gchar *cfgfile = g_build_filename (dir, ".config", "xnote", "default-style", NULL);
	g_unlink (cfgfile);
	g_free (cfgfile);

	gchar *xnotedir = g_build_filename (dir, ".config", "xnote", NULL);
	g_rmdir (xnotedir);
	g_free (xnotedir);

	gchar *cfgdir = g_build_filename (dir, ".config", NULL);
	g_rmdir (cfgdir);
	g_free (cfgdir);

	/* Also remove any autostart dir the settings init may have created */
	gchar *autostartdir = g_build_filename (dir, ".config", "autostart", NULL);
	g_rmdir (autostartdir);
	g_free (autostartdir);

	g_rmdir (dir);
	g_clear_pointer (&test_config_dir, g_free);
}

/* ---- tests ---- */

static void
test_registry_count (void)
{
	/* The registry must have exactly 19 entries (all simple bool/uint props). */
	gsize n;
	xpad_settings_get_registry (&n);
	g_assert_cmpuint (n, ==, 19);
}

static void
test_registry_fio_keys (void)
{
	/* All expected fio key strings must be present in the registry.
	   These are the on-disk keys -- any change here breaks existing configs.

	   sticky_on_start, hide_from_taskbar and hide_from_task_switcher lost
	   their Preferences rows in 2.4.0 (no Wayland client can set those WM
	   hints) but MUST stay in the registry: they keep round-tripping so a
	   downgrade to 2.3.x finds its settings intact. Do not "clean them up"
	   because the UI no longer shows them -- same contract as
	   autostart_wait_systray. */
	const gchar *expected_keys[] = {
		"decorations", "height", "width", "confirm_destroy", "edit_lock",
		"sticky_on_start", "tray_enabled", "tray_click_configuration",
		"toolbar", "auto_hide_toolbar", "scrollbar", "random_color",
		"autostart_wait_systray", "autostart_delay", "autostart_new_pad",
		"autostart_display_pads", "hide_from_taskbar", "hide_from_task_switcher",
		"line_numbering",
	};

	gsize n;
	const SettingEntry *reg = xpad_settings_get_registry (&n);

	for (guint j = 0; j < G_N_ELEMENTS (expected_keys); j++) {
		gboolean found = FALSE;
		for (gsize i = 0; i < n; i++) {
			if (strcmp (reg[i].fio_key, expected_keys[j]) == 0) {
				found = TRUE;
				break;
			}
		}
		if (!found)
			g_test_message ("Missing fio key: %s", expected_keys[j]);
		g_assert_true (found);
	}
}

static void
test_bool_props_round_trip (void)
{
	/* For every known bool property: set to non-default, save (via set_property),
	   create a fresh instance (load_from_file runs), assert value preserved. */
	struct { const gchar *name; gboolean def; } bool_props[] = {
		{ "has-decorations",          FALSE },
		{ "confirm-destroy",          TRUE  },
		{ "edit-lock",                FALSE },
		{ "tray-enabled",             TRUE  },
		{ "has-toolbar",              TRUE  },
		{ "autohide-toolbar",         TRUE  },
		{ "has-scrollbar",            TRUE  },
		{ "random-color",             TRUE  },
		{ "autostart-wait-systray",   TRUE  },
		{ "autostart-new-pad",        FALSE },
		{ "autostart-sticky",         FALSE },
		{ "hide-from-taskbar",        FALSE },
		{ "hide-from-task-switcher",  FALSE },
		{ "line-numbering",           FALSE },
	};

	gchar *tmpdir = setup_temp_home ();

	for (guint i = 0; i < G_N_ELEMENTS (bool_props); i++) {
		/* Remove any stale config file from previous iteration */
		gchar *cfgfile = g_build_filename (tmpdir, ".config", "xnote", "default-style", NULL);
		g_unlink (cfgfile);
		g_free (cfgfile);

		gboolean non_default = !bool_props[i].def;

		XpadSettings *s1 = xpad_settings_new ();
		g_object_set (s1, bool_props[i].name, non_default, NULL);
		/* set_property calls save_to_file */
		g_object_unref (s1);

		XpadSettings *s2 = xpad_settings_new ();
		gboolean readback;
		g_object_get (s2, bool_props[i].name, &readback, NULL);
		if (readback != non_default)
			g_test_message ("FAIL %s: expected %d got %d", bool_props[i].name, non_default, readback);
		g_assert_cmpint (readback, ==, non_default);
		g_object_unref (s2);
	}

	remove_temp_home (tmpdir);
	g_free (tmpdir);
}

static void
test_uint_props_round_trip (void)
{
	struct { const gchar *name; guint non_default; } uint_props[] = {
		{ "width",                    320 },
		{ "height",                   240 },
		{ "tray-click-configuration",   2 },
		{ "autostart-delay",            5 },
		{ "autostart-display-pads",     1 },
	};

	gchar *tmpdir = setup_temp_home ();

	for (guint i = 0; i < G_N_ELEMENTS (uint_props); i++) {
		gchar *cfgfile = g_build_filename (tmpdir, ".config", "xnote", "default-style", NULL);
		g_unlink (cfgfile);
		g_free (cfgfile);

		XpadSettings *s1 = xpad_settings_new ();
		g_object_set (s1, uint_props[i].name, uint_props[i].non_default, NULL);
		g_object_unref (s1);

		XpadSettings *s2 = xpad_settings_new ();
		guint readback;
		g_object_get (s2, uint_props[i].name, &readback, NULL);
		if (readback != uint_props[i].non_default)
			g_test_message ("FAIL %s: expected %u got %u", uint_props[i].name, uint_props[i].non_default, readback);
		g_assert_cmpuint (readback, ==, uint_props[i].non_default);
		g_object_unref (s2);
	}

	remove_temp_home (tmpdir);
	g_free (tmpdir);
}

static void
test_tray_disabled_preserves_startup_display (void)
{
	gchar *tmpdir = setup_temp_home ();

	for (guint mode = 1; mode <= 2; mode++) {
		XpadSettings *settings = xpad_settings_new ();
		guint stored;

		g_object_set (settings,
			"tray-enabled", TRUE,
			"autostart-display-pads", mode,
			NULL);
		g_assert_cmpuint (
			xpad_settings_get_effective_startup_display (settings), ==, mode);

		g_object_set (settings, "tray-enabled", FALSE, NULL);
		g_object_get (settings, "autostart-display-pads", &stored, NULL);
		g_assert_cmpuint (stored, ==, mode);
		g_assert_cmpuint (
			xpad_settings_get_effective_startup_display (settings), ==, 0);
		g_object_unref (settings);

		/* The stored choice must survive both a tray off/on transition and a
		   process restart while the tray is disabled. */
		settings = xpad_settings_new ();
		gboolean tray_enabled = TRUE;
		g_object_get (settings,
			"tray-enabled", &tray_enabled,
			"autostart-display-pads", &stored,
			NULL);
		g_assert_false (tray_enabled);
		g_assert_cmpuint (stored, ==, mode);
		g_assert_cmpuint (
			xpad_settings_get_effective_startup_display (settings), ==, 0);

		g_object_set (settings, "tray-enabled", TRUE, NULL);
		g_object_get (settings, "autostart-display-pads", &stored, NULL);
		g_assert_cmpuint (stored, ==, mode);
		g_assert_cmpuint (
			xpad_settings_get_effective_startup_display (settings), ==, mode);
		g_object_unref (settings);
	}

	remove_temp_home (tmpdir);
	g_free (tmpdir);
}

static void
test_color_and_font_props_round_trip (void)
{
	gchar *tmpdir = setup_temp_home ();

	XpadSettings *settings = xpad_settings_new ();
	GdkRGBA set_text = { 0.1, 0.2, 0.3, 1.0 };
	GdkRGBA set_back = { 0.9, 0.8, 0.7, 1.0 };
	GdkRGBA *got_text = NULL;
	GdkRGBA *got_back = NULL;
	gchar *got_font = NULL;

	g_object_set (settings,
		"text-color", &set_text,
		"back-color", &set_back,
		"fontname", "Monospace 13",
		NULL);

	g_object_get (settings,
		"text-color", &got_text,
		"back-color", &got_back,
		"fontname", &got_font,
		NULL);
	g_assert_nonnull (got_text);
	g_assert_nonnull (got_back);
	g_assert_true (gdk_rgba_equal (got_text, &set_text));
	g_assert_true (gdk_rgba_equal (got_back, &set_back));
	g_assert_cmpstr (got_font, ==, "Monospace 13");
	gdk_rgba_free (got_text);
	gdk_rgba_free (got_back);
	g_free (got_font);

	/* Setting to NULL (theme default) must return NULL cleanly on get. */
	g_object_set (settings,
		"text-color", NULL,
		"back-color", NULL,
		"fontname", NULL,
		NULL);
	got_text = (GdkRGBA *) 0x1;
	got_back = (GdkRGBA *) 0x1;
	got_font = (gchar *) 0x1;
	g_object_get (settings,
		"text-color", &got_text,
		"back-color", &got_back,
		"fontname", &got_font,
		NULL);
	g_assert_null (got_text);
	g_assert_null (got_back);
	g_assert_null (got_font);

	g_object_unref (settings);
	remove_temp_home (tmpdir);
	g_free (tmpdir);
}

int
main (int argc, char **argv)
{
	g_test_init (&argc, &argv, NULL);

	g_test_add_func ("/settings/registry_count",        test_registry_count);
	g_test_add_func ("/settings/registry_fio_keys",     test_registry_fio_keys);
	g_test_add_func ("/settings/bool_props_round_trip", test_bool_props_round_trip);
	g_test_add_func ("/settings/uint_props_round_trip", test_uint_props_round_trip);
	g_test_add_func ("/settings/tray_disabled_preserves_startup_display",
	                 test_tray_disabled_preserves_startup_display);
	g_test_add_func ("/settings/color_and_font_props_round_trip",
	                 test_color_and_font_props_round_trip);

	return g_test_run ();
}
