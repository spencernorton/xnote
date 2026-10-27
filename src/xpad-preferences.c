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

#include <string.h>
#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include <adwaita.h>

#include "xpad-preferences.h"
#include "xpad-app.h"
#include "xpad-backup.h"
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

	/* Backup page */
	GCancellable *backup_cancellable;
	gboolean backup_updating_ui;
	gboolean backup_busy;
	gboolean backup_initialised;
	gboolean backup_key_cached;
	gboolean backup_unlock_required;
	gboolean backup_gdrive_configured;
	gboolean backup_dropbox_configured;
	GtkWidget *backup_target_row;        /* AdwComboRow */
	GtkWidget *backup_service_row;       /* AdwActionRow */
	GtkWidget *backup_signin_button;     /* GtkButton */
	GtkWidget *backup_disconnect_button; /* GtkButton */
	GtkWidget *backup_custom_row;        /* AdwEntryRow */
	GtkWidget *backup_encryption_row;    /* AdwActionRow */
	GtkWidget *backup_setup_button;      /* GtkButton */
	GtkWidget *backup_unlock_button;     /* GtkButton */
	GtkWidget *backup_rotate_button;     /* GtkButton */
	GtkWidget *backup_status_row;        /* AdwActionRow */
	GtkWidget *backup_now_button;        /* GtkButton */
	GtkWidget *backup_timer_row;         /* AdwSwitchRow */
	gulong backup_target_handler;
	gulong backup_timer_handler;

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

/* ---- Backup page helpers and async subprocess callbacks ---- */

static void backup_refresh_status (XpadPreferences *pref);

static void
secure_zero_string (gchar *str)
{
	if (!str)
		return;
	volatile gchar *p = (volatile gchar *) str;
	while (*p)
		*p++ = '\0';
}

static void
show_backup_alert (XpadPreferences *pref, const gchar *heading, const gchar *body)
{
	AdwDialog *alert = adw_alert_dialog_new (heading, body ? body : "");
	adw_alert_dialog_add_response (ADW_ALERT_DIALOG (alert), "ok", _("OK"));
	adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (alert), "ok");
	adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (alert), "ok");
	adw_dialog_present (alert, GTK_WIDGET (pref));
}

typedef struct {
	XpadPreferences *pref;
	gchar *recovery_code;
} RecoveryAlertData;

static void
on_recovery_alert_response (AdwAlertDialog *dialog, const gchar *response, gpointer user_data)
{
	(void) dialog;
	RecoveryAlertData *data = (RecoveryAlertData *) user_data;
	if (g_strcmp0 (response, "copy") == 0 && data->recovery_code) {
		GdkDisplay *display = gtk_widget_get_display (GTK_WIDGET (data->pref));
		if (display) {
			GdkClipboard *clipboard = gdk_display_get_clipboard (display);
			gdk_clipboard_set_text (clipboard, data->recovery_code);
		}
	}
	secure_zero_string (data->recovery_code);
	g_free (data->recovery_code);
	g_free (data);
}

static gchar *
extract_recovery_code (const gchar *stdout_buf)
{
	if (!stdout_buf)
		return NULL;
	gchar **lines = g_strsplit (stdout_buf, "\n", -1);
	gchar *found = NULL;
	for (gsize i = 0; lines[i] != NULL; i++) {
		gchar *trimmed = g_strstrip (lines[i]);
		gsize len = strlen (trimmed);
		/* Recovery code is 7 groups of 8 base32 chars separated by 6 hyphens = 62 chars */
		if (len == 62 && trimmed[8] == '-' && trimmed[17] == '-' && trimmed[26] == '-') {
			found = g_strdup (trimmed);
			break;
		}
	}
	g_strfreev (lines);
	return found;
}

static void
show_recovery_code_dialog (XpadPreferences *pref, const gchar *recovery_code)
{
	AdwDialog *alert = adw_alert_dialog_new (
		_("Save Your One-Time Recovery Code"),
		_("Write this recovery code down and store it separately from your passphrase. "
		  "It is shown only once and is the only way to recover encrypted backups if you forget your passphrase."));

	GtkWidget *code_label = gtk_label_new (recovery_code);
	gtk_label_set_selectable (GTK_LABEL (code_label), TRUE);
	gtk_widget_add_css_class (code_label, "monospace");
	gtk_widget_set_margin_top (code_label, 8);
	gtk_widget_set_margin_bottom (code_label, 8);
	adw_alert_dialog_set_extra_child (ADW_ALERT_DIALOG (alert), code_label);

	adw_alert_dialog_add_response (ADW_ALERT_DIALOG (alert), "close", _("Close"));
	adw_alert_dialog_add_response (ADW_ALERT_DIALOG (alert), "copy", _("Copy Recovery Code"));
	adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (alert), "copy", ADW_RESPONSE_SUGGESTED);
	adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (alert), "copy");
	adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (alert), "close");

	RecoveryAlertData *data = g_new0 (RecoveryAlertData, 1);
	data->pref = pref;
	data->recovery_code = g_strdup (recovery_code);
	g_signal_connect (alert, "response", G_CALLBACK (on_recovery_alert_response), data);
	adw_dialog_present (alert, GTK_WIDGET (pref));
}

static void
backup_update_target_rows_for_selection (XpadPreferences *pref, guint selected)
{
	if (selected == 1) {
		/* Google Drive */
		gtk_widget_set_visible (pref->priv->backup_service_row, TRUE);
		gtk_widget_set_visible (pref->priv->backup_custom_row, FALSE);
		adw_preferences_row_set_title (ADW_PREFERENCES_ROW (pref->priv->backup_service_row), _("Google Drive account"));
		if (pref->priv->backup_gdrive_configured) {
			adw_action_row_set_subtitle (ADW_ACTION_ROW (pref->priv->backup_service_row),
				_("Signed in — encrypted recovery kits sync to xnote-gdrive:xnote-backup"));
			gtk_button_set_label (GTK_BUTTON (pref->priv->backup_signin_button), _("Reconnect…"));
			gtk_widget_remove_css_class (pref->priv->backup_signin_button, "suggested-action");
			gtk_widget_set_visible (pref->priv->backup_disconnect_button, TRUE);
		} else {
			adw_action_row_set_subtitle (ADW_ACTION_ROW (pref->priv->backup_service_row),
				_("Not signed in — authorize in your web browser"));
			gtk_button_set_label (GTK_BUTTON (pref->priv->backup_signin_button), _("Sign In with Google Drive…"));
			gtk_widget_add_css_class (pref->priv->backup_signin_button, "suggested-action");
			gtk_widget_set_visible (pref->priv->backup_disconnect_button, FALSE);
		}
	} else if (selected == 2) {
		/* Dropbox */
		gtk_widget_set_visible (pref->priv->backup_service_row, TRUE);
		gtk_widget_set_visible (pref->priv->backup_custom_row, FALSE);
		adw_preferences_row_set_title (ADW_PREFERENCES_ROW (pref->priv->backup_service_row), _("Dropbox account"));
		if (pref->priv->backup_dropbox_configured) {
			adw_action_row_set_subtitle (ADW_ACTION_ROW (pref->priv->backup_service_row),
				_("Signed in — encrypted recovery kits sync to xnote-dropbox:xnote-backup"));
			gtk_button_set_label (GTK_BUTTON (pref->priv->backup_signin_button), _("Reconnect…"));
			gtk_widget_remove_css_class (pref->priv->backup_signin_button, "suggested-action");
			gtk_widget_set_visible (pref->priv->backup_disconnect_button, TRUE);
		} else {
			adw_action_row_set_subtitle (ADW_ACTION_ROW (pref->priv->backup_service_row),
				_("Not signed in — authorize in your web browser"));
			gtk_button_set_label (GTK_BUTTON (pref->priv->backup_signin_button), _("Sign In with Dropbox…"));
			gtk_widget_add_css_class (pref->priv->backup_signin_button, "suggested-action");
			gtk_widget_set_visible (pref->priv->backup_disconnect_button, FALSE);
		}
	} else if (selected == 3) {
		/* Custom folder or rclone remote */
		gtk_widget_set_visible (pref->priv->backup_service_row, FALSE);
		gtk_widget_set_visible (pref->priv->backup_custom_row, TRUE);
	} else {
		/* Local Only */
		gtk_widget_set_visible (pref->priv->backup_service_row, FALSE);
		gtk_widget_set_visible (pref->priv->backup_custom_row, FALSE);
	}
}

static void
on_backup_status_finished (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
	GSubprocess *proc = G_SUBPROCESS (source_object);
	gchar *stdout_buf = NULL;
	gchar *stderr_buf = NULL;
	GError *error = NULL;

	if (!g_subprocess_communicate_utf8_finish (proc, res, &stdout_buf, &stderr_buf, &error)) {
		if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
			g_error_free (error);
			return;
		}
		g_error_free (error);
		g_free (stdout_buf);
		g_free (stderr_buf);
		return;
	}

	XpadPreferences *pref = XPAD_PREFERENCES (user_data);
	if (!g_subprocess_get_successful (proc) || !stdout_buf) {
		g_free (stdout_buf);
		g_free (stderr_buf);
		return;
	}

	gboolean initialised = FALSE;
	gboolean key_cached = FALSE;
	gboolean unlock_required = FALSE;
	gint local_snapshots = 0;
	guint64 head_generation = 0;
	gchar *cloud_remote = g_strdup ("");
	gchar *target_mode = g_strdup ("local");
	gboolean gdrive_configured = FALSE;
	gboolean dropbox_configured = FALSE;
	gchar *last_success_utc = g_strdup ("");
	gchar *last_error = g_strdup ("");

	gchar **lines = g_strsplit (stdout_buf, "\n", -1);
	for (gsize i = 0; lines[i] != NULL; i++) {
		gchar *eq = strchr (lines[i], '=');
		if (!eq)
			continue;
		*eq = '\0';
		const gchar *k = lines[i];
		const gchar *v = eq + 1;

		if (g_strcmp0 (k, "initialised") == 0)
			initialised = (g_strcmp0 (v, "true") == 0);
		else if (g_strcmp0 (k, "key_cached") == 0)
			key_cached = (g_strcmp0 (v, "true") == 0);
		else if (g_strcmp0 (k, "unlock_required") == 0)
			unlock_required = (g_strcmp0 (v, "true") == 0);
		else if (g_strcmp0 (k, "local_snapshots") == 0)
			local_snapshots = (gint) g_ascii_strtoll (v, NULL, 10);
		else if (g_strcmp0 (k, "head_generation") == 0)
			head_generation = g_ascii_strtoull (v, NULL, 10);
		else if (g_strcmp0 (k, "cloud_remote") == 0) {
			g_free (cloud_remote);
			cloud_remote = g_strdup (v);
		} else if (g_strcmp0 (k, "target_mode") == 0) {
			g_free (target_mode);
			target_mode = g_strdup (v);
		} else if (g_strcmp0 (k, "gdrive_configured") == 0)
			gdrive_configured = (g_strcmp0 (v, "true") == 0);
		else if (g_strcmp0 (k, "dropbox_configured") == 0)
			dropbox_configured = (g_strcmp0 (v, "true") == 0);
		else if (g_strcmp0 (k, "last_success_utc") == 0) {
			g_free (last_success_utc);
			last_success_utc = g_strdup (v);
		} else if (g_strcmp0 (k, "last_error") == 0) {
			g_free (last_error);
			last_error = g_strdup (v);
		}
	}
	g_strfreev (lines);

	pref->priv->backup_initialised = initialised;
	pref->priv->backup_key_cached = key_cached;
	pref->priv->backup_unlock_required = unlock_required;
	pref->priv->backup_gdrive_configured = gdrive_configured;
	pref->priv->backup_dropbox_configured = dropbox_configured;

	pref->priv->backup_updating_ui = TRUE;

	guint target_idx = 0;
	if (g_strcmp0 (target_mode, "gdrive") == 0)
		target_idx = 1;
	else if (g_strcmp0 (target_mode, "dropbox") == 0)
		target_idx = 2;
	else if (g_strcmp0 (target_mode, "custom") == 0)
		target_idx = 3;

	adw_combo_row_set_selected (ADW_COMBO_ROW (pref->priv->backup_target_row), target_idx);
	if (target_idx == 3 && cloud_remote && *cloud_remote)
		gtk_editable_set_text (GTK_EDITABLE (pref->priv->backup_custom_row), cloud_remote);

	backup_update_target_rows_for_selection (pref, target_idx);

	if (!initialised) {
		adw_action_row_set_subtitle (ADW_ACTION_ROW (pref->priv->backup_encryption_row),
			_("Not set up — create a passphrase to enable encrypted backups"));
		gtk_widget_set_visible (pref->priv->backup_setup_button, TRUE);
		gtk_widget_set_visible (pref->priv->backup_unlock_button, FALSE);
		gtk_widget_set_visible (pref->priv->backup_rotate_button, FALSE);
	} else if (!key_cached || unlock_required) {
		adw_action_row_set_subtitle (ADW_ACTION_ROW (pref->priv->backup_encryption_row),
			_("Locked — unlock with your passphrase or recovery code to back up"));
		gtk_widget_set_visible (pref->priv->backup_setup_button, FALSE);
		gtk_widget_set_visible (pref->priv->backup_unlock_button, TRUE);
		gtk_widget_set_visible (pref->priv->backup_rotate_button, FALSE);
	} else {
		adw_action_row_set_subtitle (ADW_ACTION_ROW (pref->priv->backup_encryption_row),
			_("Unlocked — master key cached in login keyring"));
		gtk_widget_set_visible (pref->priv->backup_setup_button, FALSE);
		gtk_widget_set_visible (pref->priv->backup_unlock_button, FALSE);
		gtk_widget_set_visible (pref->priv->backup_rotate_button, TRUE);
	}

	GString *status_text = g_string_new (NULL);
	g_string_printf (status_text, _("%d local snapshots (generation %lu)"),
		local_snapshots, (unsigned long) head_generation);
	if (cloud_remote && *cloud_remote) {
		if (last_error && *last_error) {
			g_string_append_printf (status_text, _(" • Last cloud error: %s"), last_error);
		} else if (last_success_utc && *last_success_utc) {
			g_string_append_printf (status_text, _(" • Last cloud sync: %s"), last_success_utc);
		} else {
			g_string_append (status_text, _(" • Cloud sync pending first backup"));
		}
	} else {
		g_string_append (status_text, _(" • Local-only mode"));
	}
	adw_action_row_set_subtitle (ADW_ACTION_ROW (pref->priv->backup_status_row), status_text->str);
	g_string_free (status_text, TRUE);

	gtk_widget_set_sensitive (pref->priv->backup_now_button,
		initialised && key_cached && !unlock_required && !pref->priv->backup_busy);

	pref->priv->backup_updating_ui = FALSE;

	g_free (cloud_remote);
	g_free (target_mode);
	g_free (last_success_utc);
	g_free (last_error);
	g_free (stdout_buf);
	g_free (stderr_buf);
}

static void
on_backup_timer_check_finished (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
	GSubprocess *proc = G_SUBPROCESS (source_object);
	gchar *stdout_buf = NULL;
	gchar *stderr_buf = NULL;
	GError *error = NULL;

	if (!g_subprocess_communicate_utf8_finish (proc, res, &stdout_buf, &stderr_buf, &error)) {
		if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
			g_error_free (error);
			return;
		}
		g_error_free (error);
		g_free (stdout_buf);
		g_free (stderr_buf);
		return;
	}

	XpadPreferences *pref = XPAD_PREFERENCES (user_data);
	gboolean enabled = g_subprocess_get_successful (proc);

	pref->priv->backup_updating_ui = TRUE;
	adw_switch_row_set_active (ADW_SWITCH_ROW (pref->priv->backup_timer_row), enabled);
	pref->priv->backup_updating_ui = FALSE;

	g_free (stdout_buf);
	g_free (stderr_buf);
}

static void
backup_refresh_status (XpadPreferences *pref)
{
	const gchar *helper = xpad_backup_get_helper_path ();
	if (!helper) {
		adw_action_row_set_subtitle (ADW_ACTION_ROW (pref->priv->backup_status_row),
			_("xnote-cloud-backup helper not found in PATH"));
		gtk_widget_set_sensitive (pref->priv->backup_target_row, FALSE);
		gtk_widget_set_sensitive (pref->priv->backup_encryption_row, FALSE);
		gtk_widget_set_sensitive (pref->priv->backup_now_button, FALSE);
		return;
	}

	const gchar *argv[] = { helper, "status", "--porcelain", NULL };
	GError *error = NULL;
	GSubprocess *proc = g_subprocess_newv (argv,
		G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE, &error);
	if (proc) {
		g_subprocess_communicate_utf8_async (proc, NULL, pref->priv->backup_cancellable,
			on_backup_status_finished, pref);
		g_object_unref (proc);
	} else {
		g_clear_error (&error);
	}

	gchar *systemctl = g_find_program_in_path ("systemctl");
	if (systemctl) {
		const gchar *t_argv[] = { systemctl, "--user", "is-enabled", "xnote-cloud-backup.timer", NULL };
		GSubprocess *t_proc = g_subprocess_newv (t_argv,
			G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE, NULL);
		if (t_proc) {
			g_subprocess_communicate_utf8_async (t_proc, NULL, pref->priv->backup_cancellable,
				on_backup_timer_check_finished, pref);
			g_object_unref (t_proc);
		}
		g_free (systemctl);
	}
}

static void
on_backup_generic_cmd_finished (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
	GSubprocess *proc = G_SUBPROCESS (source_object);
	gchar *stdout_buf = NULL;
	gchar *stderr_buf = NULL;
	GError *error = NULL;

	if (!g_subprocess_communicate_utf8_finish (proc, res, &stdout_buf, &stderr_buf, &error)) {
		if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
			g_error_free (error);
			return;
		}
		XpadPreferences *pref = XPAD_PREFERENCES (user_data);
		pref->priv->backup_busy = FALSE;
		show_backup_alert (pref, _("Backup Operation Failed"), error->message);
		g_error_free (error);
		backup_refresh_status (pref);
		return;
	}

	XpadPreferences *pref = XPAD_PREFERENCES (user_data);
	pref->priv->backup_busy = FALSE;

	if (!g_subprocess_get_successful (proc)) {
		const gchar *msg = (stderr_buf && *stderr_buf) ? g_strstrip (stderr_buf) : _("The backup helper returned an error.");
		show_backup_alert (pref, _("Backup Operation Failed"), msg);
	}

	g_free (stdout_buf);
	g_free (stderr_buf);
	backup_refresh_status (pref);
}

static void
backup_run_helper_async (XpadPreferences *pref, const gchar * const *argv, const gchar *stdin_text, GAsyncReadyCallback callback, gpointer cb_data)
{
	GError *error = NULL;
	GSubprocessFlags flags = G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE;
	if (stdin_text)
		flags |= G_SUBPROCESS_FLAGS_STDIN_PIPE;

	GSubprocess *proc = g_subprocess_newv ((const gchar * const *) argv, flags, &error);
	if (!proc) {
		pref->priv->backup_busy = FALSE;
		show_backup_alert (pref, _("Failed to Start Backup Helper"), error ? error->message : "");
		g_clear_error (&error);
		return;
	}
	g_subprocess_communicate_utf8_async (proc, stdin_text, pref->priv->backup_cancellable,
		callback ? callback : on_backup_generic_cmd_finished, cb_data ? cb_data : pref);
	g_object_unref (proc);
}

static void
on_backup_target_changed (GObject *object, GParamSpec *pspec, XpadPreferences *pref)
{
	(void) pspec;
	if (pref->priv->backup_updating_ui)
		return;

	guint selected = adw_combo_row_get_selected (ADW_COMBO_ROW (object));
	backup_update_target_rows_for_selection (pref, selected);

	const gchar *helper = xpad_backup_get_helper_path ();
	if (!helper)
		return;

	if (selected == 0) {
		const gchar *argv[] = { helper, "set-target", "local", NULL };
		backup_run_helper_async (pref, argv, NULL, NULL, NULL);
	} else if (selected == 1 && pref->priv->backup_gdrive_configured) {
		const gchar *argv[] = { helper, "set-target", "gdrive", NULL };
		backup_run_helper_async (pref, argv, NULL, NULL, NULL);
	} else if (selected == 2 && pref->priv->backup_dropbox_configured) {
		const gchar *argv[] = { helper, "set-target", "dropbox", NULL };
		backup_run_helper_async (pref, argv, NULL, NULL, NULL);
	}
}

static void
on_backup_custom_apply (AdwEntryRow *entry_row, XpadPreferences *pref)
{
	const gchar *helper = xpad_backup_get_helper_path ();
	if (!helper)
		return;

	const gchar *text = gtk_editable_get_text (GTK_EDITABLE (entry_row));
	if (!text || !*text) {
		show_backup_alert (pref, _("Invalid Backup Target"), _("Enter a local directory path (/path/to/folder) or an rclone remote (remote:path)."));
		return;
	}
	const gchar *argv[] = { helper, "set-target", "custom", text, NULL };
	backup_run_helper_async (pref, argv, NULL, NULL, NULL);
}

static void
on_backup_signin_clicked (GtkButton *button, XpadPreferences *pref)
{
	(void) button;
	const gchar *helper = xpad_backup_get_helper_path ();
	if (!helper || pref->priv->backup_busy)
		return;

	guint selected = adw_combo_row_get_selected (ADW_COMBO_ROW (pref->priv->backup_target_row));
	const gchar *provider = NULL;
	if (selected == 1)
		provider = "gdrive";
	else if (selected == 2)
		provider = "dropbox";
	else
		return;

	pref->priv->backup_busy = TRUE;
	adw_action_row_set_subtitle (ADW_ACTION_ROW (pref->priv->backup_service_row),
		_("Opening web browser for authorization — complete sign-in in your browser…"));
	gtk_widget_set_sensitive (pref->priv->backup_signin_button, FALSE);

	const gchar *argv[] = { helper, "connect", provider, NULL };
	backup_run_helper_async (pref, argv, NULL, NULL, NULL);
	gtk_widget_set_sensitive (pref->priv->backup_signin_button, TRUE);
}

static void
on_backup_disconnect_clicked (GtkButton *button, XpadPreferences *pref)
{
	(void) button;
	const gchar *helper = xpad_backup_get_helper_path ();
	if (!helper || pref->priv->backup_busy)
		return;

	guint selected = adw_combo_row_get_selected (ADW_COMBO_ROW (pref->priv->backup_target_row));
	const gchar *provider = NULL;
	if (selected == 1)
		provider = "gdrive";
	else if (selected == 2)
		provider = "dropbox";
	else
		return;

	pref->priv->backup_busy = TRUE;
	const gchar *argv[] = { helper, "disconnect", provider, NULL };
	backup_run_helper_async (pref, argv, NULL, NULL, NULL);
}

static void
on_backup_now_clicked (GtkButton *button, XpadPreferences *pref)
{
	(void) button;
	const gchar *helper = xpad_backup_get_helper_path ();
	if (!helper || pref->priv->backup_busy)
		return;

	pref->priv->backup_busy = TRUE;
	gtk_widget_set_sensitive (pref->priv->backup_now_button, FALSE);
	adw_action_row_set_subtitle (ADW_ACTION_ROW (pref->priv->backup_status_row),
		_("Encrypting snapshot and syncing recovery kit…"));

	const gchar *argv[] = { helper, "backup", NULL };
	backup_run_helper_async (pref, argv, NULL, NULL, NULL);
}

static void
on_backup_timer_toggle_finished (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
	GSubprocess *proc = G_SUBPROCESS (source_object);
	gchar *stdout_buf = NULL;
	gchar *stderr_buf = NULL;
	GError *error = NULL;

	if (!g_subprocess_communicate_utf8_finish (proc, res, &stdout_buf, &stderr_buf, &error)) {
		if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
			g_error_free (error);
			return;
		}
		g_error_free (error);
		return;
	}
	XpadPreferences *pref = XPAD_PREFERENCES (user_data);
	g_free (stdout_buf);
	g_free (stderr_buf);
	backup_refresh_status (pref);
}

static void
on_backup_timer_notify_active (GObject *object, GParamSpec *pspec, XpadPreferences *pref)
{
	(void) pspec;
	if (pref->priv->backup_updating_ui)
		return;

	gchar *systemctl = g_find_program_in_path ("systemctl");
	if (!systemctl)
		return;

	gboolean active = adw_switch_row_get_active (ADW_SWITCH_ROW (object));
	const gchar *verb = active ? "enable" : "disable";
	const gchar *argv[] = { systemctl, "--user", verb, "--now", "xnote-cloud-backup.timer", NULL };
	GSubprocess *proc = g_subprocess_newv (argv,
		G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE, NULL);
	if (proc) {
		g_subprocess_communicate_utf8_async (proc, NULL, pref->priv->backup_cancellable,
			on_backup_timer_toggle_finished, pref);
		g_object_unref (proc);
	}
	g_free (systemctl);
}

/* ---- Encryption Passphrase Dialogs (Init / Unlock / Rotate) ---- */

typedef struct {
	XpadPreferences *pref;
	AdwDialog *dialog;
	GtkWidget *pass1_row;
	GtkWidget *pass2_row;
	GtkWidget *pass3_row;
	GtkWidget *recovery_switch_row;
	GtkWidget *status_label;
	GtkWidget *submit_button;
} BackupAuthDialogData;

static void
on_backup_init_finished (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
	GSubprocess *proc = G_SUBPROCESS (source_object);
	gchar *stdout_buf = NULL;
	gchar *stderr_buf = NULL;
	GError *error = NULL;

	if (!g_subprocess_communicate_utf8_finish (proc, res, &stdout_buf, &stderr_buf, &error)) {
		if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
			g_error_free (error);
			return;
		}
		XpadPreferences *pref = XPAD_PREFERENCES (user_data);
		pref->priv->backup_busy = FALSE;
		show_backup_alert (pref, _("Encryption Setup Failed"), error->message);
		g_error_free (error);
		backup_refresh_status (pref);
		return;
	}

	XpadPreferences *pref = XPAD_PREFERENCES (user_data);
	pref->priv->backup_busy = FALSE;

	if (!g_subprocess_get_successful (proc)) {
		const gchar *msg = (stderr_buf && *stderr_buf) ? g_strstrip (stderr_buf) : _("Failed to initialize encryption.");
		show_backup_alert (pref, _("Encryption Setup Failed"), msg);
	} else {
		gchar *code = extract_recovery_code (stdout_buf);
		if (code) {
			show_recovery_code_dialog (pref, code);
			secure_zero_string (code);
			g_free (code);
		}
	}

	secure_zero_string (stdout_buf);
	g_free (stdout_buf);
	g_free (stderr_buf);
	backup_refresh_status (pref);
}

static void
on_setup_dialog_submit (GtkButton *button, BackupAuthDialogData *data)
{
	(void) button;
	const gchar *p1 = gtk_editable_get_text (GTK_EDITABLE (data->pass1_row));
	const gchar *p2 = gtk_editable_get_text (GTK_EDITABLE (data->pass2_row));

	if (!p1 || !*p1) {
		gtk_label_set_text (GTK_LABEL (data->status_label), _("Passphrase must not be empty."));
		gtk_widget_set_visible (data->status_label, TRUE);
		return;
	}
	if (g_strcmp0 (p1, p2) != 0) {
		gtk_label_set_text (GTK_LABEL (data->status_label), _("Passphrases do not match."));
		gtk_widget_set_visible (data->status_label, TRUE);
		return;
	}

	const gchar *helper = xpad_backup_get_helper_path ();
	if (!helper)
		return;

	gchar *stdin_payload = g_strdup_printf ("%s\n%s\n", p1, p2);
	gtk_editable_set_text (GTK_EDITABLE (data->pass1_row), "");
	gtk_editable_set_text (GTK_EDITABLE (data->pass2_row), "");
	adw_dialog_close (data->dialog);

	data->pref->priv->backup_busy = TRUE;
	adw_action_row_set_subtitle (ADW_ACTION_ROW (data->pref->priv->backup_encryption_row),
		_("Deriving Argon2id encryption key (this takes a few seconds)…"));

	const gchar *argv[] = { helper, "init", NULL };
	backup_run_helper_async (data->pref, argv, stdin_payload, on_backup_init_finished, data->pref);
	secure_zero_string (stdin_payload);
	g_free (stdin_payload);
}

static void
on_backup_setup_clicked (GtkButton *button, XpadPreferences *pref)
{
	(void) button;
	if (pref->priv->backup_busy)
		return;

	AdwDialog *dlg = adw_dialog_new ();
	adw_dialog_set_title (dlg, _("Set Up Encrypted Backups"));
	adw_dialog_set_content_width (dlg, 440);

	AdwToolbarView *tv = ADW_TOOLBAR_VIEW (adw_toolbar_view_new ());
	adw_toolbar_view_add_top_bar (tv, adw_header_bar_new ());

	GtkWidget *vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
	gtk_widget_set_margin_top (vbox, 12);
	gtk_widget_set_margin_bottom (vbox, 16);
	gtk_widget_set_margin_start (vbox, 16);
	gtk_widget_set_margin_end (vbox, 16);

	GtkWidget *desc = gtk_label_new (
		_("Backups are encrypted end-to-end on this device using Argon2id and XChaCha20-Poly1305. "
		  "Neither Google Drive nor Dropbox can read your notes. Choose a passphrase below."));
	gtk_label_set_wrap (GTK_LABEL (desc), TRUE);
	gtk_label_set_xalign (GTK_LABEL (desc), 0.0f);
	gtk_box_append (GTK_BOX (vbox), desc);

	AdwPreferencesGroup *grp = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
	GtkWidget *pass1 = adw_password_entry_row_new ();
	adw_preferences_row_set_title (ADW_PREFERENCES_ROW (pass1), _("Passphrase"));
	adw_preferences_group_add (grp, pass1);

	GtkWidget *pass2 = adw_password_entry_row_new ();
	adw_preferences_row_set_title (ADW_PREFERENCES_ROW (pass2), _("Confirm passphrase"));
	adw_preferences_group_add (grp, pass2);
	gtk_box_append (GTK_BOX (vbox), GTK_WIDGET (grp));

	GtkWidget *err_label = gtk_label_new ("");
	gtk_widget_add_css_class (err_label, "error");
	gtk_widget_set_visible (err_label, FALSE);
	gtk_box_append (GTK_BOX (vbox), err_label);

	GtkWidget *submit = gtk_button_new_with_label (_("Initialize Encryption"));
	gtk_widget_add_css_class (submit, "suggested-action");
	gtk_widget_add_css_class (submit, "pill");
	gtk_widget_set_halign (submit, GTK_ALIGN_END);
	gtk_box_append (GTK_BOX (vbox), submit);

	adw_toolbar_view_set_content (tv, vbox);
	adw_dialog_set_child (dlg, GTK_WIDGET (tv));

	BackupAuthDialogData *data = g_new0 (BackupAuthDialogData, 1);
	data->pref = pref;
	data->dialog = dlg;
	data->pass1_row = pass1;
	data->pass2_row = pass2;
	data->status_label = err_label;
	data->submit_button = submit;

	g_signal_connect_data (submit, "clicked", G_CALLBACK (on_setup_dialog_submit),
		data, (GClosureNotify) g_free, 0);

	adw_dialog_present (dlg, GTK_WIDGET (pref));
}

static void
on_unlock_dialog_submit (GtkButton *button, BackupAuthDialogData *data)
{
	(void) button;
	const gchar *secret = gtk_editable_get_text (GTK_EDITABLE (data->pass1_row));
	if (!secret || !*secret) {
		gtk_label_set_text (GTK_LABEL (data->status_label), _("Enter your passphrase or recovery code."));
		gtk_widget_set_visible (data->status_label, TRUE);
		return;
	}

	const gchar *helper = xpad_backup_get_helper_path ();
	if (!helper)
		return;

	gboolean use_recovery = adw_switch_row_get_active (ADW_SWITCH_ROW (data->recovery_switch_row));
	gchar *stdin_payload = g_strdup_printf ("%s\n", secret);
	gtk_editable_set_text (GTK_EDITABLE (data->pass1_row), "");
	adw_dialog_close (data->dialog);

	data->pref->priv->backup_busy = TRUE;
	adw_action_row_set_subtitle (ADW_ACTION_ROW (data->pref->priv->backup_encryption_row),
		_("Deriving Argon2id key and caching in login keyring…"));

	if (use_recovery) {
		const gchar *argv[] = { helper, "unlock", "--recovery", NULL };
		backup_run_helper_async (data->pref, argv, stdin_payload, NULL, NULL);
	} else {
		const gchar *argv[] = { helper, "unlock", NULL };
		backup_run_helper_async (data->pref, argv, stdin_payload, NULL, NULL);
	}
	secure_zero_string (stdin_payload);
	g_free (stdin_payload);
}

static void
on_backup_unlock_clicked (GtkButton *button, XpadPreferences *pref)
{
	(void) button;
	if (pref->priv->backup_busy)
		return;

	AdwDialog *dlg = adw_dialog_new ();
	adw_dialog_set_title (dlg, _("Unlock Encrypted Backups"));
	adw_dialog_set_content_width (dlg, 420);

	AdwToolbarView *tv = ADW_TOOLBAR_VIEW (adw_toolbar_view_new ());
	adw_toolbar_view_add_top_bar (tv, adw_header_bar_new ());

	GtkWidget *vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
	gtk_widget_set_margin_top (vbox, 12);
	gtk_widget_set_margin_bottom (vbox, 16);
	gtk_widget_set_margin_start (vbox, 16);
	gtk_widget_set_margin_end (vbox, 16);

	AdwPreferencesGroup *grp = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
	GtkWidget *pass1 = adw_password_entry_row_new ();
	adw_preferences_row_set_title (ADW_PREFERENCES_ROW (pass1), _("Passphrase or recovery code"));
	adw_preferences_group_add (grp, pass1);

	GtkWidget *rec_switch = adw_switch_row_new ();
	adw_preferences_row_set_title (ADW_PREFERENCES_ROW (rec_switch), _("Use one-time recovery code"));
	adw_preferences_group_add (grp, rec_switch);
	gtk_box_append (GTK_BOX (vbox), GTK_WIDGET (grp));

	GtkWidget *err_label = gtk_label_new ("");
	gtk_widget_add_css_class (err_label, "error");
	gtk_widget_set_visible (err_label, FALSE);
	gtk_box_append (GTK_BOX (vbox), err_label);

	GtkWidget *submit = gtk_button_new_with_label (_("Unlock"));
	gtk_widget_add_css_class (submit, "suggested-action");
	gtk_widget_add_css_class (submit, "pill");
	gtk_widget_set_halign (submit, GTK_ALIGN_END);
	gtk_box_append (GTK_BOX (vbox), submit);

	adw_toolbar_view_set_content (tv, vbox);
	adw_dialog_set_child (dlg, GTK_WIDGET (tv));

	BackupAuthDialogData *data = g_new0 (BackupAuthDialogData, 1);
	data->pref = pref;
	data->dialog = dlg;
	data->pass1_row = pass1;
	data->recovery_switch_row = rec_switch;
	data->status_label = err_label;
	data->submit_button = submit;

	g_signal_connect_data (submit, "clicked", G_CALLBACK (on_unlock_dialog_submit),
		data, (GClosureNotify) g_free, 0);

	adw_dialog_present (dlg, GTK_WIDGET (pref));
}

static void
on_rotate_dialog_submit (GtkButton *button, BackupAuthDialogData *data)
{
	(void) button;
	const gchar *cur = gtk_editable_get_text (GTK_EDITABLE (data->pass1_row));
	const gchar *np1 = gtk_editable_get_text (GTK_EDITABLE (data->pass2_row));
	const gchar *np2 = gtk_editable_get_text (GTK_EDITABLE (data->pass3_row));

	if (!cur || !*cur || !np1 || !*np1) {
		gtk_label_set_text (GTK_LABEL (data->status_label), _("All passphrase fields are required."));
		gtk_widget_set_visible (data->status_label, TRUE);
		return;
	}
	if (g_strcmp0 (np1, np2) != 0) {
		gtk_label_set_text (GTK_LABEL (data->status_label), _("New passphrases do not match."));
		gtk_widget_set_visible (data->status_label, TRUE);
		return;
	}

	const gchar *helper = xpad_backup_get_helper_path ();
	if (!helper)
		return;

	gchar *stdin_payload = g_strdup_printf ("%s\n%s\n%s\n", cur, np1, np2);
	gtk_editable_set_text (GTK_EDITABLE (data->pass1_row), "");
	gtk_editable_set_text (GTK_EDITABLE (data->pass2_row), "");
	gtk_editable_set_text (GTK_EDITABLE (data->pass3_row), "");
	adw_dialog_close (data->dialog);

	data->pref->priv->backup_busy = TRUE;
	adw_action_row_set_subtitle (ADW_ACTION_ROW (data->pref->priv->backup_encryption_row),
		_("Re-wrapping master key under new passphrase…"));

	const gchar *argv[] = { helper, "rotate-passphrase", NULL };
	backup_run_helper_async (data->pref, argv, stdin_payload, NULL, NULL);
	secure_zero_string (stdin_payload);
	g_free (stdin_payload);
}

static void
on_backup_rotate_clicked (GtkButton *button, XpadPreferences *pref)
{
	(void) button;
	if (pref->priv->backup_busy)
		return;

	AdwDialog *dlg = adw_dialog_new ();
	adw_dialog_set_title (dlg, _("Change Backup Passphrase"));
	adw_dialog_set_content_width (dlg, 440);

	AdwToolbarView *tv = ADW_TOOLBAR_VIEW (adw_toolbar_view_new ());
	adw_toolbar_view_add_top_bar (tv, adw_header_bar_new ());

	GtkWidget *vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
	gtk_widget_set_margin_top (vbox, 12);
	gtk_widget_set_margin_bottom (vbox, 16);
	gtk_widget_set_margin_start (vbox, 16);
	gtk_widget_set_margin_end (vbox, 16);

	AdwPreferencesGroup *grp = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
	GtkWidget *pass1 = adw_password_entry_row_new ();
	adw_preferences_row_set_title (ADW_PREFERENCES_ROW (pass1), _("Current passphrase"));
	adw_preferences_group_add (grp, pass1);

	GtkWidget *pass2 = adw_password_entry_row_new ();
	adw_preferences_row_set_title (ADW_PREFERENCES_ROW (pass2), _("New passphrase"));
	adw_preferences_group_add (grp, pass2);

	GtkWidget *pass3 = adw_password_entry_row_new ();
	adw_preferences_row_set_title (ADW_PREFERENCES_ROW (pass3), _("Confirm new passphrase"));
	adw_preferences_group_add (grp, pass3);
	gtk_box_append (GTK_BOX (vbox), GTK_WIDGET (grp));

	GtkWidget *err_label = gtk_label_new ("");
	gtk_widget_add_css_class (err_label, "error");
	gtk_widget_set_visible (err_label, FALSE);
	gtk_box_append (GTK_BOX (vbox), err_label);

	GtkWidget *submit = gtk_button_new_with_label (_("Change Passphrase"));
	gtk_widget_add_css_class (submit, "suggested-action");
	gtk_widget_add_css_class (submit, "pill");
	gtk_widget_set_halign (submit, GTK_ALIGN_END);
	gtk_box_append (GTK_BOX (vbox), submit);

	adw_toolbar_view_set_content (tv, vbox);
	adw_dialog_set_child (dlg, GTK_WIDGET (tv));

	BackupAuthDialogData *data = g_new0 (BackupAuthDialogData, 1);
	data->pref = pref;
	data->dialog = dlg;
	data->pass1_row = pass1;
	data->pass2_row = pass2;
	data->pass3_row = pass3;
	data->status_label = err_label;
	data->submit_button = submit;

	g_signal_connect_data (submit, "clicked", G_CALLBACK (on_rotate_dialog_submit),
		data, (GClosureNotify) g_free, 0);

	adw_dialog_present (dlg, GTK_WIDGET (pref));
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

	/* =========================================================
	 * PAGE: Backup
	 * ========================================================= */
	pref->priv->backup_cancellable = g_cancellable_new ();

	AdwPreferencesPage *page_backup = ADW_PREFERENCES_PAGE (adw_preferences_page_new ());
	adw_preferences_page_set_title (page_backup, _("Backup"));
	adw_preferences_page_set_icon_name (page_backup, "drive-harddisk-symbolic");
	adw_preferences_dialog_add (ADW_PREFERENCES_DIALOG (pref), page_backup);

	AdwPreferencesGroup *grp_backup_dest = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
	adw_preferences_group_set_title (grp_backup_dest, _("Backup Destination"));
	adw_preferences_group_set_description (grp_backup_dest,
		_("All local snapshots and cloud recovery kits are encrypted end-to-end on this device "
		  "(Argon2id + XChaCha20-Poly1305) before leaving your computer."));
	adw_preferences_page_add (page_backup, grp_backup_dest);

	const gchar *backup_target_items[] = {
		N_("Local Only"),
		N_("Google Drive"),
		N_("Dropbox"),
		N_("Custom Folder or Remote")
	};
	GtkStringList *backup_target_list = make_string_list (backup_target_items, G_N_ELEMENTS (backup_target_items));
	pref->priv->backup_target_row = adw_combo_row_new ();
	adw_preferences_row_set_title (ADW_PREFERENCES_ROW (pref->priv->backup_target_row), _("Backup target"));
	adw_combo_row_set_model (ADW_COMBO_ROW (pref->priv->backup_target_row), G_LIST_MODEL (backup_target_list));
	adw_preferences_group_add (grp_backup_dest, pref->priv->backup_target_row);

	pref->priv->backup_service_row = adw_action_row_new ();
	adw_preferences_row_set_title (ADW_PREFERENCES_ROW (pref->priv->backup_service_row), _("Cloud account"));
	GtkWidget *service_btn_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_widget_set_valign (service_btn_box, GTK_ALIGN_CENTER);

	pref->priv->backup_signin_button = gtk_button_new_with_label (_("Sign In…"));
	gtk_widget_set_valign (pref->priv->backup_signin_button, GTK_ALIGN_CENTER);
	gtk_box_append (GTK_BOX (service_btn_box), pref->priv->backup_signin_button);

	pref->priv->backup_disconnect_button = gtk_button_new_with_label (_("Disconnect"));
	gtk_widget_set_valign (pref->priv->backup_disconnect_button, GTK_ALIGN_CENTER);
	gtk_widget_set_visible (pref->priv->backup_disconnect_button, FALSE);
	gtk_box_append (GTK_BOX (service_btn_box), pref->priv->backup_disconnect_button);

	adw_action_row_add_suffix (ADW_ACTION_ROW (pref->priv->backup_service_row), service_btn_box);
	gtk_widget_set_visible (pref->priv->backup_service_row, FALSE);
	adw_preferences_group_add (grp_backup_dest, pref->priv->backup_service_row);

	pref->priv->backup_custom_row = adw_entry_row_new ();
	adw_preferences_row_set_title (ADW_PREFERENCES_ROW (pref->priv->backup_custom_row),
		_("Folder path or rclone remote (e.g. /mnt/backup/xnote or remote:path)"));
	adw_entry_row_set_show_apply_button (ADW_ENTRY_ROW (pref->priv->backup_custom_row), TRUE);
	gtk_widget_set_visible (pref->priv->backup_custom_row, FALSE);
	adw_preferences_group_add (grp_backup_dest, pref->priv->backup_custom_row);

	AdwPreferencesGroup *grp_backup_enc = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
	adw_preferences_group_set_title (grp_backup_enc, _("End-to-End Encryption"));
	adw_preferences_page_add (page_backup, grp_backup_enc);

	pref->priv->backup_encryption_row = adw_action_row_new ();
	adw_preferences_row_set_title (ADW_PREFERENCES_ROW (pref->priv->backup_encryption_row), _("Encryption key"));
	adw_action_row_set_subtitle (ADW_ACTION_ROW (pref->priv->backup_encryption_row), _("Checking encryption status…"));

	GtkWidget *enc_btn_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_widget_set_valign (enc_btn_box, GTK_ALIGN_CENTER);

	pref->priv->backup_setup_button = gtk_button_new_with_label (_("Set Up Encryption…"));
	gtk_widget_add_css_class (pref->priv->backup_setup_button, "suggested-action");
	gtk_widget_set_valign (pref->priv->backup_setup_button, GTK_ALIGN_CENTER);
	gtk_widget_set_visible (pref->priv->backup_setup_button, FALSE);
	gtk_box_append (GTK_BOX (enc_btn_box), pref->priv->backup_setup_button);

	pref->priv->backup_unlock_button = gtk_button_new_with_label (_("Unlock…"));
	gtk_widget_add_css_class (pref->priv->backup_unlock_button, "suggested-action");
	gtk_widget_set_valign (pref->priv->backup_unlock_button, GTK_ALIGN_CENTER);
	gtk_widget_set_visible (pref->priv->backup_unlock_button, FALSE);
	gtk_box_append (GTK_BOX (enc_btn_box), pref->priv->backup_unlock_button);

	pref->priv->backup_rotate_button = gtk_button_new_with_label (_("Change Passphrase…"));
	gtk_widget_set_valign (pref->priv->backup_rotate_button, GTK_ALIGN_CENTER);
	gtk_widget_set_visible (pref->priv->backup_rotate_button, FALSE);
	gtk_box_append (GTK_BOX (enc_btn_box), pref->priv->backup_rotate_button);

	adw_action_row_add_suffix (ADW_ACTION_ROW (pref->priv->backup_encryption_row), enc_btn_box);
	adw_preferences_group_add (grp_backup_enc, pref->priv->backup_encryption_row);

	AdwPreferencesGroup *grp_backup_status = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
	adw_preferences_group_set_title (grp_backup_status, _("Status & Schedule"));
	adw_preferences_page_add (page_backup, grp_backup_status);

	pref->priv->backup_status_row = adw_action_row_new ();
	adw_preferences_row_set_title (ADW_PREFERENCES_ROW (pref->priv->backup_status_row), _("Snapshot status"));
	adw_action_row_set_subtitle (ADW_ACTION_ROW (pref->priv->backup_status_row), _("Reading backup status…"));

	pref->priv->backup_now_button = gtk_button_new_with_label (_("Back Up Now"));
	gtk_widget_set_valign (pref->priv->backup_now_button, GTK_ALIGN_CENTER);
	gtk_widget_set_sensitive (pref->priv->backup_now_button, FALSE);
	adw_action_row_add_suffix (ADW_ACTION_ROW (pref->priv->backup_status_row), pref->priv->backup_now_button);
	adw_preferences_group_add (grp_backup_status, pref->priv->backup_status_row);

	pref->priv->backup_timer_row = adw_switch_row_new ();
	adw_preferences_row_set_title (ADW_PREFERENCES_ROW (pref->priv->backup_timer_row),
		_("Automatic hourly background backup"));
	adw_action_row_set_subtitle (ADW_ACTION_ROW (pref->priv->backup_timer_row),
		_("Runs xnote-cloud-backup.timer in the background"));
	adw_preferences_group_add (grp_backup_status, pref->priv->backup_timer_row);

	pref->priv->backup_target_handler = g_signal_connect (pref->priv->backup_target_row,
		"notify::selected", G_CALLBACK (on_backup_target_changed), pref);
	g_signal_connect (pref->priv->backup_signin_button, "clicked", G_CALLBACK (on_backup_signin_clicked), pref);
	g_signal_connect (pref->priv->backup_disconnect_button, "clicked", G_CALLBACK (on_backup_disconnect_clicked), pref);
	g_signal_connect (pref->priv->backup_custom_row, "apply", G_CALLBACK (on_backup_custom_apply), pref);
	g_signal_connect (pref->priv->backup_setup_button, "clicked", G_CALLBACK (on_backup_setup_clicked), pref);
	g_signal_connect (pref->priv->backup_unlock_button, "clicked", G_CALLBACK (on_backup_unlock_clicked), pref);
	g_signal_connect (pref->priv->backup_rotate_button, "clicked", G_CALLBACK (on_backup_rotate_clicked), pref);
	g_signal_connect (pref->priv->backup_now_button, "clicked", G_CALLBACK (on_backup_now_clicked), pref);
	pref->priv->backup_timer_handler = g_signal_connect (pref->priv->backup_timer_row,
		"notify::active", G_CALLBACK (on_backup_timer_notify_active), pref);

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
	backup_refresh_status (pref);
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

	if (pref->priv->backup_cancellable) {
		g_cancellable_cancel (pref->priv->backup_cancellable);
		g_clear_object (&pref->priv->backup_cancellable);
	}

	/* Every settings notification is connected with pref as its callback data,
	   so one matched disconnect covers the handlers that do not need IDs for
	   temporary blocking as well as those that do. */
	if (pref->priv->settings) {
		g_signal_handlers_disconnect_matched (pref->priv->settings, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, pref);
		g_clear_object (&pref->priv->settings);
	}

	G_OBJECT_CLASS (xpad_preferences_parent_class)->finalize (object);
}
