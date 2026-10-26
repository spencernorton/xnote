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

/*
 * XNote Preferences — rebuilt as an AdwPreferencesDialog (libadwaita 1.6+).
 *
 * Most settings are wired with g_object_bind_property
 * (G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE), which replaces the
 * previous ~48 hand-written change_X/notify_X callbacks and the associated
 * signal-block/unblock dance.
 *
 * Special cases that cannot use a simple bind are handled explicitly:
 *   - autostart-xpad:  derives state from a filesystem symlink, not a stored
 *     field; a simple state-set callback is kept.
 *   - font:  NULL vs. string means "use theme font" -- a radio pair plus the
 *     font button, not bindable directly to a switch.
 *   - color: NULL vs. GdkRGBA means "use theme color" -- same pattern.
 *   - tray-enabled sensitivity: disables tray-click and autostart-display-pads
 *     rows when tray is off; wired with one-way property bindings.
 *   - has-toolbar sensitivity: disables autohide-toolbar when toolbar is off.
 *
 * The legacy autostart-wait-systray setting is deliberately not presented:
 * its X-LXQt-Need-Tray launcher key has no effect on XNote's GNOME target, and
 * rewriting the packaged autostart symlink into a user-owned file made the
 * launcher drift after upgrades. The config key remains readable/writable in
 * XpadSettings for backwards compatibility.
 */

#include "../config.h"

#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include <adwaita.h>

#include "xpad-preferences.h"
#include "xpad-app.h"
#include "xpad-pad-group.h"
#include "xpad-styling-helpers.h"

/* ---- Private struct ---- */

struct XpadPreferencesPrivate
{
	XpadSettings *settings;

	/* Layout page — font/color special widgets */
	GtkWidget *antifontcheck;
	GtkWidget *fontcheck;
	GtkWidget *fontbutton;
	GtkWidget *anticolorcheck;
	GtkWidget *colorcheck;
	GtkWidget *colorbox;       /* container holding text/back buttons */
	GtkWidget *textbutton;
	GtkWidget *backbutton;
	GtkWidget *random_color_row; /* AdwSwitchRow */

	/* View page — sensitivity-managed rows */
	GtkWidget *autohide_toolbar_row;  /* AdwSwitchRow; sensitive only when toolbar on */

	/* Tray page — sensitivity-managed rows */
	GtkWidget *tray_click_row;        /* AdwComboRow */
	GtkWidget *autostart_display_pads_row; /* AdwComboRow */

	/* Startup page */
	GtkWidget *autostart_xpad_row;    /* AdwSwitchRow */

	/* Signal handler IDs for the special-case callbacks */
	gulong fontcheck_handler;
	gulong antifontcheck_handler;
	gulong font_handler;
	gulong colorcheck_handler;
	gulong anticolorcheck_handler;
	gulong text_handler;
	gulong back_handler;
	gulong autostart_xpad_handler;
	gulong notify_font_handler;
	gulong notify_text_handler;
	gulong notify_back_handler;
};

G_DEFINE_TYPE_WITH_PRIVATE (XpadPreferences, xpad_preferences, ADW_TYPE_PREFERENCES_DIALOG)

static void xpad_preferences_set_property (GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec);
static void xpad_preferences_get_property (GObject *object, guint prop_id, GValue *value, GParamSpec *pspec);
static void xpad_preferences_constructed (GObject *object);
static void xpad_preferences_finalize (GObject *object);

static GtkWidget *_xpad_preferences = NULL;

enum
{
	PROP_0,
	PROP_SETTINGS,
	N_PROPERTIES
};

static GParamSpec *obj_prop[N_PROPERTIES] = { NULL, };

/* ---- Public API ---- */

void
xpad_preferences_open (XpadSettings *settings)
{
	if (!_xpad_preferences)
	{
		_xpad_preferences = GTK_WIDGET (g_object_new (XPAD_TYPE_PREFERENCES, "settings", settings, NULL));
		g_signal_connect_swapped (_xpad_preferences, "destroy", G_CALLBACK (g_nullify_pointer), &_xpad_preferences);
	}

	/* AdwDialog is presented relative to a parent window; use the active
	   application window (or NULL — adw_dialog_present handles that). */
	GtkApplication *app = GTK_APPLICATION (g_application_get_default ());
	GtkWindow *parent = app ? gtk_application_get_active_window (app) : NULL;
	adw_dialog_present (ADW_DIALOG (_xpad_preferences), parent ? GTK_WIDGET (parent) : NULL);
}

/* ---- GObject boilerplate ---- */

static void
xpad_preferences_class_init (XpadPreferencesClass *klass)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

	gobject_class->constructed = xpad_preferences_constructed;
	gobject_class->set_property = xpad_preferences_set_property;
	gobject_class->get_property = xpad_preferences_get_property;
	gobject_class->finalize = xpad_preferences_finalize;

	obj_prop[PROP_SETTINGS] = g_param_spec_pointer ("settings", "Xpad settings", "Xpad global settings", G_PARAM_READWRITE | G_PARAM_CONSTRUCT);

	g_object_class_install_properties (gobject_class, N_PROPERTIES, obj_prop);
}

static void
xpad_preferences_init (XpadPreferences *pref)
{
	pref->priv = xpad_preferences_get_instance_private (pref);
}

/* ---- Special-case callbacks (cannot be replaced by g_object_bind_property) ---- */

static void
change_font_check (GtkCheckButton *button, XpadPreferences *pref)
{
	g_signal_handler_block (pref->priv->settings, pref->priv->notify_font_handler);

	if (gtk_check_button_get_active (button)) {
		gchar *font = gtk_font_chooser_get_font (GTK_FONT_CHOOSER (pref->priv->fontbutton));
		g_object_set (pref->priv->settings, "fontname", font, NULL);
		g_free (font);
	} else {
		g_object_set (pref->priv->settings, "fontname", NULL, NULL);
	}

	gtk_widget_set_sensitive (pref->priv->fontbutton, gtk_check_button_get_active (button));
	g_signal_handler_unblock (pref->priv->settings, pref->priv->notify_font_handler);
}

static void
change_font_face (GtkFontChooser *button, XpadPreferences *pref)
{
	gchar *font = gtk_font_chooser_get_font (button);
	g_signal_handler_block (pref->priv->settings, pref->priv->notify_font_handler);
	g_object_set (pref->priv->settings, "fontname", font, NULL);
	g_signal_handler_unblock (pref->priv->settings, pref->priv->notify_font_handler);
	g_free (font);
}

static void
change_color_check (GtkCheckButton *button, XpadPreferences *pref)
{
	g_signal_handler_block (pref->priv->settings, pref->priv->notify_text_handler);
	g_signal_handler_block (pref->priv->settings, pref->priv->notify_back_handler);

	if (gtk_check_button_get_active (button)) {
		GdkRGBA text_color, back_color;
		gtk_color_chooser_get_rgba (GTK_COLOR_CHOOSER (pref->priv->textbutton), &text_color);
		gtk_color_chooser_get_rgba (GTK_COLOR_CHOOSER (pref->priv->backbutton), &back_color);
		g_object_set (pref->priv->settings, "text-color", &text_color, "back-color", &back_color, NULL);
	} else {
		g_object_set (pref->priv->settings, "text-color", NULL, "back-color", NULL, NULL);
	}

	gtk_widget_set_sensitive (pref->priv->colorbox, gtk_check_button_get_active (button));

	g_signal_handler_unblock (pref->priv->settings, pref->priv->notify_text_handler);
	g_signal_handler_unblock (pref->priv->settings, pref->priv->notify_back_handler);
}

static void
change_text_color (GtkColorChooser *chooser, XpadPreferences *pref)
{
	GdkRGBA text_color = {0, 0, 0, 0};
	gtk_color_chooser_get_rgba (chooser, &text_color);

	g_signal_handler_block (pref->priv->settings, pref->priv->notify_text_handler);
	g_object_set (pref->priv->settings, "text-color", &text_color, NULL);
	g_signal_handler_unblock (pref->priv->settings, pref->priv->notify_text_handler);
}

static void
change_back_color (GtkColorChooser *chooser, XpadPreferences *pref)
{
	GdkRGBA back_color = {0, 0, 0, 0};
	gtk_color_chooser_get_rgba (chooser, &back_color);

	g_signal_handler_block (pref->priv->settings, pref->priv->notify_back_handler);
	g_object_set (pref->priv->settings, "back-color", &back_color, NULL);
	g_signal_handler_unblock (pref->priv->settings, pref->priv->notify_back_handler);
}

static gboolean
change_autostart_xpad (GtkSwitch *sw, gboolean state, XpadPreferences *pref)
{
	gboolean actual;

	g_object_set (pref->priv->settings, "autostart-xpad", state, NULL);
	/* This property is derived from the launcher on disk. A failed mkdir,
	   symlink, rename, or delete must be visible: acknowledge the state that
	   actually exists, not the state the user requested. */
	g_object_get (pref->priv->settings, "autostart-xpad", &actual, NULL);
	g_signal_handler_block (sw, pref->priv->autostart_xpad_handler);
	gtk_switch_set_active (sw, actual);
	gtk_switch_set_state (sw, actual);
	g_signal_handler_unblock (sw, pref->priv->autostart_xpad_handler);
	return TRUE;
}

/* notify callbacks for settings → UI updates on the special-cased fields */

static void
notify_fontname (XpadPreferences *pref)
{
	gchar *fontname = NULL;
	g_object_get (pref->priv->settings, "fontname", &fontname, NULL);

	g_signal_handler_block (pref->priv->fontbutton, pref->priv->font_handler);
	g_signal_handler_block (pref->priv->fontcheck, pref->priv->fontcheck_handler);
	g_signal_handler_block (pref->priv->antifontcheck, pref->priv->antifontcheck_handler);

	if (fontname)
	{
		gtk_check_button_set_active (GTK_CHECK_BUTTON (pref->priv->fontcheck), TRUE);
		gtk_widget_set_sensitive (pref->priv->fontbutton, TRUE);
		gtk_font_chooser_set_font (GTK_FONT_CHOOSER (pref->priv->fontbutton), fontname);
		g_free (fontname);
	}
	else
	{
		gtk_widget_set_sensitive (pref->priv->fontbutton, FALSE);
		gtk_check_button_set_active (GTK_CHECK_BUTTON (pref->priv->antifontcheck), TRUE);
	}

	g_signal_handler_unblock (pref->priv->antifontcheck, pref->priv->antifontcheck_handler);
	g_signal_handler_unblock (pref->priv->fontcheck, pref->priv->fontcheck_handler);
	g_signal_handler_unblock (pref->priv->fontbutton, pref->priv->font_handler);
}

static void
notify_text_color (XpadPreferences *pref)
{
	GdkRGBA *text_color = NULL;
	g_object_get (pref->priv->settings, "text-color", &text_color, NULL);

	g_signal_handler_block (pref->priv->textbutton, pref->priv->text_handler);
	g_signal_handler_block (pref->priv->colorcheck, pref->priv->colorcheck_handler);
	g_signal_handler_block (pref->priv->anticolorcheck, pref->priv->anticolorcheck_handler);

	if (text_color)
	{
		gtk_check_button_set_active (GTK_CHECK_BUTTON (pref->priv->colorcheck), TRUE);
		gtk_widget_set_sensitive (pref->priv->colorbox, TRUE);
		gtk_color_chooser_set_rgba (GTK_COLOR_CHOOSER (pref->priv->textbutton), text_color);
		gdk_rgba_free (text_color);
	}
	else
	{
		gtk_widget_set_sensitive (pref->priv->colorbox, FALSE);
		gtk_check_button_set_active (GTK_CHECK_BUTTON (pref->priv->anticolorcheck), TRUE);
	}

	g_signal_handler_unblock (pref->priv->anticolorcheck, pref->priv->anticolorcheck_handler);
	g_signal_handler_unblock (pref->priv->colorcheck, pref->priv->colorcheck_handler);
	g_signal_handler_unblock (pref->priv->textbutton, pref->priv->text_handler);
}

static void
notify_back_color (XpadPreferences *pref)
{
	GdkRGBA *back_color = NULL;
	g_object_get (pref->priv->settings, "back-color", &back_color, NULL);

	g_signal_handler_block (pref->priv->backbutton, pref->priv->back_handler);
	g_signal_handler_block (pref->priv->colorcheck, pref->priv->colorcheck_handler);
	g_signal_handler_block (pref->priv->anticolorcheck, pref->priv->anticolorcheck_handler);

	if (back_color)
	{
		gtk_check_button_set_active (GTK_CHECK_BUTTON (pref->priv->colorcheck), TRUE);
		gtk_widget_set_sensitive (pref->priv->colorbox, TRUE);
		gtk_color_chooser_set_rgba (GTK_COLOR_CHOOSER (pref->priv->backbutton), back_color);
		gdk_rgba_free (back_color);
	}
	else
	{
		gtk_widget_set_sensitive (pref->priv->colorbox, FALSE);
		gtk_check_button_set_active (GTK_CHECK_BUTTON (pref->priv->anticolorcheck), TRUE);
	}

	g_signal_handler_unblock (pref->priv->anticolorcheck, pref->priv->anticolorcheck_handler);
	g_signal_handler_unblock (pref->priv->colorcheck, pref->priv->colorcheck_handler);
	g_signal_handler_unblock (pref->priv->backbutton, pref->priv->back_handler);
}

static void
notify_has_toolbar (XpadPreferences *pref)
{
	gboolean value;
	g_object_get (pref->priv->settings, "has-toolbar", &value, NULL);
	gtk_widget_set_sensitive (pref->priv->autohide_toolbar_row, value);
}

static void
notify_autostart_xpad (XpadPreferences *pref)
{
	gboolean value;
	g_object_get (pref->priv->settings, "autostart-xpad", &value, NULL);

	/* Update the switch without re-firing our callback */
	GtkWidget *sw = adw_action_row_get_activatable_widget (
	                  ADW_ACTION_ROW (pref->priv->autostart_xpad_row));
	if (sw) {
		g_signal_handler_block (sw, pref->priv->autostart_xpad_handler);
		gtk_switch_set_active (GTK_SWITCH (sw), value);
		g_signal_handler_unblock (sw, pref->priv->autostart_xpad_handler);
	}

}

/* ---- Helper: build a GtkStringList from a static list of translatable items ---- */

static GtkStringList *
make_string_list (const gchar * const *items, guint n)
{
	GtkStringList *list = gtk_string_list_new (NULL);
	for (guint i = 0; i < n; i++)
		gtk_string_list_append (list, _(items[i]));
	return list;
}

/* ---- Build the UI ---- */

static void
xpad_preferences_constructed (GObject *object)
{
	G_OBJECT_CLASS (xpad_preferences_parent_class)->constructed (object);

	XpadPreferences *pref = XPAD_PREFERENCES (object);
	XpadSettings *settings = pref->priv->settings;

	/* Title */
	adw_dialog_set_title (ADW_DIALOG (pref), _("XNote Preferences"));

	/* =========================================================
	 * PAGE: View
	 * ========================================================= */
	AdwPreferencesPage *page_view = ADW_PREFERENCES_PAGE (adw_preferences_page_new ());
	adw_preferences_page_set_title (page_view, _("View"));
	adw_preferences_page_set_icon_name (page_view, "preferences-desktop-symbolic");
	adw_preferences_dialog_add (ADW_PREFERENCES_DIALOG (pref), page_view);

	AdwPreferencesGroup *grp_toolbar = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
	adw_preferences_group_set_title (grp_toolbar, _("Toolbar"));
	adw_preferences_page_add (page_view, grp_toolbar);

	GtkWidget *has_toolbar_row = adw_switch_row_new ();
	adw_preferences_row_set_title (ADW_PREFERENCES_ROW (has_toolbar_row), _("Show toolbar"));
	adw_preferences_group_add (grp_toolbar, has_toolbar_row);

	pref->priv->autohide_toolbar_row = adw_switch_row_new ();
	adw_preferences_row_set_title (ADW_PREFERENCES_ROW (pref->priv->autohide_toolbar_row), _("Autohide toolbar"));
	adw_preferences_group_add (grp_toolbar, pref->priv->autohide_toolbar_row);

	AdwPreferencesGroup *grp_view_misc = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
	adw_preferences_group_set_title (grp_view_misc, _("Notes"));
	adw_preferences_page_add (page_view, grp_view_misc);

	GtkWidget *has_scrollbar_row = adw_switch_row_new ();
	adw_preferences_row_set_title (ADW_PREFERENCES_ROW (has_scrollbar_row), _("Show scrollbar"));
	adw_preferences_group_add (grp_view_misc, has_scrollbar_row);

	GtkWidget *has_decorations_row = adw_switch_row_new ();
	adw_preferences_row_set_title (ADW_PREFERENCES_ROW (has_decorations_row), _("Show window decorations"));
	adw_preferences_group_add (grp_view_misc, has_decorations_row);

	/* =========================================================
	 * PAGE: Layout (font, color, pad size)
	 * ========================================================= */
	AdwPreferencesPage *page_layout = ADW_PREFERENCES_PAGE (adw_preferences_page_new ());
	adw_preferences_page_set_title (page_layout, _("Layout"));
	adw_preferences_page_set_icon_name (page_layout, "applications-graphics-symbolic");
	adw_preferences_dialog_add (ADW_PREFERENCES_DIALOG (pref), page_layout);

	/* Font group */
	AdwPreferencesGroup *grp_font = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
	adw_preferences_group_set_title (grp_font, _("Font"));
	adw_preferences_page_add (page_layout, grp_font);

	/* Font uses a radio pair + button — not bindable, built as before */
	GtkWidget *font_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
	gtk_widget_set_margin_top (font_box, 6);
	gtk_widget_set_margin_bottom (font_box, 6);
	gtk_widget_set_margin_start (font_box, 12);
	gtk_widget_set_margin_end (font_box, 12);

	pref->priv->antifontcheck = gtk_check_button_new_with_mnemonic (_("Use font from theme"));
	pref->priv->fontcheck = gtk_check_button_new_with_mnemonic (_("Use this font"));
	gtk_check_button_set_group (GTK_CHECK_BUTTON (pref->priv->fontcheck), GTK_CHECK_BUTTON (pref->priv->antifontcheck));
	pref->priv->fontbutton = gtk_font_button_new ();
	gtk_font_button_set_title (GTK_FONT_BUTTON (pref->priv->fontbutton), _("Set Font"));

	GtkWidget *font_hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_box_append (GTK_BOX (font_hbox), pref->priv->fontcheck);
	gtk_box_append (GTK_BOX (font_hbox), pref->priv->fontbutton);
	gtk_box_append (GTK_BOX (font_box), pref->priv->antifontcheck);
	gtk_box_append (GTK_BOX (font_box), font_hbox);

	AdwActionRow *font_row = ADW_ACTION_ROW (adw_action_row_new ());
	adw_action_row_add_suffix (font_row, font_box);
	gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (font_row), FALSE);
	adw_preferences_group_add (grp_font, GTK_WIDGET (font_row));

	/* Color group */
	AdwPreferencesGroup *grp_color = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
	adw_preferences_group_set_title (grp_color, _("Colors"));
	adw_preferences_page_add (page_layout, grp_color);

	GtkWidget *color_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
	gtk_widget_set_margin_top (color_box, 6);
	gtk_widget_set_margin_bottom (color_box, 6);
	gtk_widget_set_margin_start (color_box, 12);
	gtk_widget_set_margin_end (color_box, 12);

	pref->priv->anticolorcheck = gtk_check_button_new_with_mnemonic (_("Use colors from theme"));
	pref->priv->colorcheck = gtk_check_button_new_with_mnemonic (_("Use these colors"));
	gtk_check_button_set_group (GTK_CHECK_BUTTON (pref->priv->colorcheck), GTK_CHECK_BUTTON (pref->priv->anticolorcheck));

	pref->priv->textbutton = gtk_color_button_new ();
	gtk_color_button_set_title (GTK_COLOR_BUTTON (pref->priv->textbutton), _("Set Foreground Color"));
	gtk_color_chooser_set_use_alpha (GTK_COLOR_CHOOSER (pref->priv->textbutton), FALSE);

	pref->priv->backbutton = gtk_color_button_new ();
	gtk_color_button_set_title (GTK_COLOR_BUTTON (pref->priv->backbutton), _("Set Background Color"));
	gtk_color_chooser_set_use_alpha (GTK_COLOR_CHOOSER (pref->priv->backbutton), TRUE);

	pref->priv->colorbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
	gtk_widget_set_margin_start (pref->priv->colorbox, 12);
	GtkWidget *text_hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
	gtk_box_append (GTK_BOX (text_hbox), gtk_label_new (_("Text")));
	gtk_box_append (GTK_BOX (text_hbox), pref->priv->textbutton);
	GtkWidget *back_hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
	gtk_box_append (GTK_BOX (back_hbox), gtk_label_new (_("Background")));
	gtk_box_append (GTK_BOX (back_hbox), pref->priv->backbutton);
	gtk_box_append (GTK_BOX (pref->priv->colorbox), text_hbox);
	gtk_box_append (GTK_BOX (pref->priv->colorbox), back_hbox);

	gtk_box_append (GTK_BOX (color_box), pref->priv->anticolorcheck);
	gtk_box_append (GTK_BOX (color_box), pref->priv->colorcheck);
	gtk_box_append (GTK_BOX (color_box), pref->priv->colorbox);

	AdwActionRow *color_row = ADW_ACTION_ROW (adw_action_row_new ());
	adw_action_row_add_suffix (color_row, color_box);
	gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (color_row), FALSE);
	adw_preferences_group_add (grp_color, GTK_WIDGET (color_row));

	pref->priv->random_color_row = adw_switch_row_new ();
	adw_preferences_row_set_title (ADW_PREFERENCES_ROW (pref->priv->random_color_row), _("Give new pads a random sticky-note color"));
	adw_preferences_group_add (grp_color, pref->priv->random_color_row);

	/* Size group */
	AdwPreferencesGroup *grp_size = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
	adw_preferences_group_set_title (grp_size, _("New pad size"));
	adw_preferences_page_add (page_layout, grp_size);

	GtkWidget *height_row = adw_spin_row_new_with_range (10, 99999, 10);
	adw_preferences_row_set_title (ADW_PREFERENCES_ROW (height_row), _("Height"));
	adw_preferences_group_add (grp_size, height_row);

	GtkWidget *width_row = adw_spin_row_new_with_range (10, 99999, 10);
	adw_preferences_row_set_title (ADW_PREFERENCES_ROW (width_row), _("Width"));
	adw_preferences_group_add (grp_size, width_row);

	/* =========================================================
	 * PAGE: Startup
	 * ========================================================= */
	AdwPreferencesPage *page_startup = ADW_PREFERENCES_PAGE (adw_preferences_page_new ());
	adw_preferences_page_set_title (page_startup, _("Startup"));
	adw_preferences_page_set_icon_name (page_startup, "system-run-symbolic");
	adw_preferences_dialog_add (ADW_PREFERENCES_DIALOG (pref), page_startup);

	AdwPreferencesGroup *grp_autostart = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
	adw_preferences_group_set_title (grp_autostart, _("Autostart"));
	adw_preferences_page_add (page_startup, grp_autostart);

	pref->priv->autostart_xpad_row = adw_switch_row_new ();
	adw_preferences_row_set_title (ADW_PREFERENCES_ROW (pref->priv->autostart_xpad_row), _("Start XNote automatically after login"));
	adw_preferences_group_add (grp_autostart, pref->priv->autostart_xpad_row);

	GtkWidget *autostart_new_pad_row = adw_switch_row_new ();
	adw_preferences_row_set_title (ADW_PREFERENCES_ROW (autostart_new_pad_row), _("Open a new empty pad"));
	adw_preferences_group_add (grp_autostart, autostart_new_pad_row);


	const gchar *delay_items[] = {
		"0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", "13", "14"
	};
	/* make_string_list takes an explicit count; gtk_string_list_new() requires a
	   NULL-terminated array and delay_items is not terminated, so calling it
	   directly over-read past the array and crashed (strlen on stack garbage). */
	GtkStringList *delay_list = make_string_list (delay_items, G_N_ELEMENTS (delay_items));
	GtkWidget *autostart_delay_row = adw_combo_row_new ();
	adw_preferences_row_set_title (ADW_PREFERENCES_ROW (autostart_delay_row), _("Delay in seconds"));
	adw_combo_row_set_model (ADW_COMBO_ROW (autostart_delay_row), G_LIST_MODEL (delay_list));
	adw_preferences_group_add (grp_autostart, autostart_delay_row);

	const gchar *display_items[] = { N_("Open all pads"), N_("Hide all pads"), N_("Restore to previous state") };
	GtkStringList *display_list = make_string_list (display_items, G_N_ELEMENTS (display_items));
	pref->priv->autostart_display_pads_row = adw_combo_row_new ();
	adw_preferences_row_set_title (ADW_PREFERENCES_ROW (pref->priv->autostart_display_pads_row), _("Display pads"));
	adw_combo_row_set_model (ADW_COMBO_ROW (pref->priv->autostart_display_pads_row), G_LIST_MODEL (display_list));
	adw_preferences_group_add (grp_autostart, pref->priv->autostart_display_pads_row);

	/* =========================================================
	 * PAGE: Tray
	 * ========================================================= */
	AdwPreferencesPage *page_tray = ADW_PREFERENCES_PAGE (adw_preferences_page_new ());
	adw_preferences_page_set_title (page_tray, _("Tray"));
	adw_preferences_page_set_icon_name (page_tray, "user-available-symbolic");
	adw_preferences_dialog_add (ADW_PREFERENCES_DIALOG (pref), page_tray);

	AdwPreferencesGroup *grp_tray = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
	adw_preferences_group_set_title (grp_tray, _("System tray icon"));
	adw_preferences_page_add (page_tray, grp_tray);

	GtkWidget *tray_enabled_row = adw_switch_row_new ();
	adw_preferences_row_set_title (ADW_PREFERENCES_ROW (tray_enabled_row), _("Enable tray icon"));
	adw_preferences_group_add (grp_tray, tray_enabled_row);

	const gchar *tray_click_items[] = { N_("Do Nothing"), N_("Toggle Show All"), N_("List of Pads"), N_("New Pad") };
	GtkStringList *tray_click_list = make_string_list (tray_click_items, G_N_ELEMENTS (tray_click_items));
	pref->priv->tray_click_row = adw_combo_row_new ();
	adw_preferences_row_set_title (ADW_PREFERENCES_ROW (pref->priv->tray_click_row), _("Tray left mouse click behavior"));
	adw_combo_row_set_model (ADW_COMBO_ROW (pref->priv->tray_click_row), G_LIST_MODEL (tray_click_list));
	adw_preferences_group_add (grp_tray, pref->priv->tray_click_row);

	/* =========================================================
	 * PAGE: Other
	 * ========================================================= */
	AdwPreferencesPage *page_other = ADW_PREFERENCES_PAGE (adw_preferences_page_new ());
	adw_preferences_page_set_title (page_other, _("Other"));
	adw_preferences_page_set_icon_name (page_other, "preferences-other-symbolic");
	adw_preferences_dialog_add (ADW_PREFERENCES_DIALOG (pref), page_other);

	AdwPreferencesGroup *grp_other = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
	adw_preferences_group_set_title (grp_other, _("Behaviour"));
	adw_preferences_page_add (page_other, grp_other);

	GtkWidget *edit_lock_row = adw_switch_row_new ();
	adw_preferences_row_set_title (ADW_PREFERENCES_ROW (edit_lock_row), _("Make pads read-only (CTRL-J)"));
	adw_preferences_group_add (grp_other, edit_lock_row);

	GtkWidget *confirm_destroy_row = adw_switch_row_new ();
	adw_preferences_row_set_title (ADW_PREFERENCES_ROW (confirm_destroy_row), _("Confirm pad deletion"));
	adw_preferences_group_add (grp_other, confirm_destroy_row);

	GtkWidget *line_numbering_row = adw_switch_row_new ();
	adw_preferences_row_set_title (ADW_PREFERENCES_ROW (line_numbering_row), _("Enable line numbering"));
	adw_preferences_group_add (grp_other, line_numbering_row);

	/* ==========================================================
	 * BINDINGS -- simple bool/uint properties
	 *
	 * AdwSwitchRow has an "active" property; AdwSpinRow has "value";
	 * AdwComboRow has "selected".  All are bidirectional + sync-create,
	 * replacing the old change_X/notify_X pairs entirely.
	 * ========================================================== */
	g_object_bind_property (settings, "has-toolbar",             has_toolbar_row,                          "active",   G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);
	g_object_bind_property (settings, "autohide-toolbar",        pref->priv->autohide_toolbar_row,         "active",   G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);
	g_object_bind_property (settings, "has-scrollbar",           has_scrollbar_row,                        "active",   G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);
	g_object_bind_property (settings, "has-decorations",         has_decorations_row,                      "active",   G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);
	g_object_bind_property (settings, "random-color",            pref->priv->random_color_row,             "active",   G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);
	g_object_bind_property (settings, "tray-enabled",            tray_enabled_row,                         "active",   G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);
	g_object_bind_property (settings, "tray-click-configuration",pref->priv->tray_click_row,               "selected", G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);
	g_object_bind_property (settings, "edit-lock",               edit_lock_row,                            "active",   G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);
	g_object_bind_property (settings, "confirm-destroy",         confirm_destroy_row,                      "active",   G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);
	g_object_bind_property (settings, "line-numbering",          line_numbering_row,                       "active",   G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);
	g_object_bind_property (settings, "autostart-new-pad",       autostart_new_pad_row,                    "active",   G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);
	g_object_bind_property (settings, "autostart-display-pads",  pref->priv->autostart_display_pads_row,  "selected", G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);
	g_object_bind_property (settings, "autostart-delay",         autostart_delay_row,                      "selected", G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);
	/* Disabling the tray makes its dependent choices unavailable, but must
	   not rewrite either stored value. Startup reachability is handled by
	   xpad_settings_get_effective_startup_display(). */
	g_object_bind_property (settings, "tray-enabled",
		pref->priv->tray_click_row, "sensitive", G_BINDING_SYNC_CREATE);
	g_object_bind_property (settings, "tray-enabled",
		pref->priv->autostart_display_pads_row, "sensitive", G_BINDING_SYNC_CREATE);

	/* Height/width: AdwSpinRow "value" is a double; settings property is guint.
	   No transform func needed: the GObject binding coerces uint to double automatically. */
	g_object_bind_property (settings, "height", height_row, "value", G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);
	g_object_bind_property (settings, "width",  width_row,  "value", G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);

	/* ==========================================================
	 * SPECIAL CALLBACKS — autostart-xpad, sticky, font, color
	 * ========================================================== */

	/* autostart-xpad: derived from filesystem; use state-set signal on the inner GtkSwitch */
	{
		GtkWidget *sw = adw_action_row_get_activatable_widget (ADW_ACTION_ROW (pref->priv->autostart_xpad_row));
		if (sw) {
			gboolean autostart_xpad;
			g_object_get (settings, "autostart-xpad", &autostart_xpad, NULL);
			gtk_switch_set_active (GTK_SWITCH (sw), autostart_xpad);
			pref->priv->autostart_xpad_handler = g_signal_connect (sw, "state-set", G_CALLBACK (change_autostart_xpad), pref);
		}
	}

	/* Font */
	{
		gchar *fontname = NULL;
		g_object_get (settings, "fontname", &fontname, NULL);

		if (fontname) {
			gtk_check_button_set_active (GTK_CHECK_BUTTON (pref->priv->fontcheck), TRUE);
			gtk_font_chooser_set_font (GTK_FONT_CHOOSER (pref->priv->fontbutton), fontname);
			g_free (fontname);
		} else {
			gtk_check_button_set_active (GTK_CHECK_BUTTON (pref->priv->antifontcheck), TRUE);
			gtk_widget_set_sensitive (pref->priv->fontbutton, FALSE);

			gchar *theme_font = NULL;
			g_object_get (gtk_settings_get_default (), "gtk-font-name", &theme_font, NULL);
			if (theme_font) {
				gtk_font_chooser_set_font (GTK_FONT_CHOOSER (pref->priv->fontbutton), theme_font);
				g_free (theme_font);
			}
		}

		pref->priv->fontcheck_handler = g_signal_connect (pref->priv->fontcheck, "toggled", G_CALLBACK (change_font_check), pref);
		pref->priv->antifontcheck_handler = g_signal_connect (pref->priv->antifontcheck, "toggled", G_CALLBACK (change_font_check), pref);
		pref->priv->font_handler = g_signal_connect (pref->priv->fontbutton, "font-set", G_CALLBACK (change_font_face), pref);
		pref->priv->notify_font_handler = g_signal_connect_swapped (settings, "notify::fontname", G_CALLBACK (notify_fontname), pref);
	}

	/* Color */
	{
		GdkRGBA *text_color = NULL, *back_color = NULL;
		GtkStyleContext *style;
		GdkRGBA theme_text_color = {0, 0, 0, 0}, theme_background_color = {0, 0, 0, 0};

		g_object_get (settings, "text-color", &text_color, "back-color", &back_color, NULL);

		style = gtk_widget_get_style_context (GTK_WIDGET (pref));
		gtk_style_context_get_color (style, &theme_text_color);
		get_background_color (style, GTK_STATE_FLAG_NORMAL, &theme_background_color);

		if (text_color) {
			gtk_color_chooser_set_rgba (GTK_COLOR_CHOOSER (pref->priv->textbutton), text_color);
			gdk_rgba_free (text_color);
		} else {
			gtk_color_chooser_set_rgba (GTK_COLOR_CHOOSER (pref->priv->textbutton), &theme_text_color);
		}

		if (back_color) {
			gtk_check_button_set_active (GTK_CHECK_BUTTON (pref->priv->colorcheck), TRUE);
			gtk_color_chooser_set_rgba (GTK_COLOR_CHOOSER (pref->priv->backbutton), back_color);
			gdk_rgba_free (back_color);
		} else {
			gtk_check_button_set_active (GTK_CHECK_BUTTON (pref->priv->anticolorcheck), TRUE);
			gtk_widget_set_sensitive (pref->priv->colorbox, FALSE);
			gtk_color_chooser_set_rgba (GTK_COLOR_CHOOSER (pref->priv->backbutton), &theme_background_color);
		}

		pref->priv->colorcheck_handler = g_signal_connect (pref->priv->colorcheck, "toggled", G_CALLBACK (change_color_check), pref);
		pref->priv->anticolorcheck_handler = g_signal_connect (pref->priv->anticolorcheck, "toggled", G_CALLBACK (change_color_check), pref);
		pref->priv->text_handler = g_signal_connect (pref->priv->textbutton, "color-set", G_CALLBACK (change_text_color), pref);
		pref->priv->back_handler = g_signal_connect (pref->priv->backbutton, "color-set", G_CALLBACK (change_back_color), pref);
		pref->priv->notify_text_handler = g_signal_connect_swapped (settings, "notify::text-color", G_CALLBACK (notify_text_color), pref);
		pref->priv->notify_back_handler = g_signal_connect_swapped (settings, "notify::back-color", G_CALLBACK (notify_back_color), pref);
	}

	/* Sensitivity notify handlers */
	g_signal_connect_swapped (settings, "notify::has-toolbar", G_CALLBACK (notify_has_toolbar), pref);
	g_signal_connect_swapped (settings, "notify::autostart-xpad", G_CALLBACK (notify_autostart_xpad), pref);

	/* Initialize derived sensitivity states */
	notify_has_toolbar (pref);
}

/* ---- GObject property plumbing ---- */

static void
xpad_preferences_set_property (GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
	XpadPreferences *pref = XPAD_PREFERENCES (object);

	switch (prop_id)
	{
	case PROP_SETTINGS:
		pref->priv->settings = g_value_get_pointer (value);
		g_object_ref (pref->priv->settings);
		break;

	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
xpad_preferences_get_property (GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
	XpadPreferences *pref = XPAD_PREFERENCES (object);

	switch (prop_id)
	{
	case PROP_SETTINGS:
		g_value_set_pointer (value, pref->priv->settings);
		break;

	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
xpad_preferences_finalize (GObject *object)
{
	XpadPreferences *pref = XPAD_PREFERENCES (object);

	/* Every settings notification is connected with pref as its callback data,
	   so one matched disconnect covers the handlers that do not need IDs for
	   temporary blocking as well as those that do. */
	if (pref->priv->settings) {
		g_signal_handlers_disconnect_matched (pref->priv->settings, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, pref);
		g_clear_object (&pref->priv->settings);
	}

	G_OBJECT_CLASS (xpad_preferences_parent_class)->finalize (object);
}
