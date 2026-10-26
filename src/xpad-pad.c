/*

Copyright (c) 2001-2007 Michael Terry
Copyright (c) 2009 Paul Ivanov
Copyright (c) 2011 Dennis Hilmar
Copyright (c) 2011 OBATA Akio
Copyright (c) 2013-2024 Arthur Borsboom
Copyright (c) 2019 Siergiej Riaguzow

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

#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include <gtksourceview/gtksource.h>

#include "constants.h"
#include "xpad-pad.h"
#include "xpad-app.h"
#include "xpad-backup.h"
#include "xpad-grip-tool-item.h"
#include "xpad-pad-properties.h"
#include "xpad-periodic.h"
#include "xpad-preferences.h"
#include "xpad-search-bar.h"
#include "xpad-text-buffer.h"
#include "xpad-text-view.h"
#include "xpad-toolbar.h"
#include "xpad-tray.h"
#include "fio.h"
#include "help.h"

struct XpadPadPrivate
{
	/* saved values */
	gint x, y;
	guint width, height;
	gchar *infoname;
	gchar *contentname;
	gboolean sticky;

	/* selected child widgets */
	GtkOverlay *text_with_search_overlay;

	/*
	 * TODO: Why everything is a GtkWidget? declare as proper child types for readability
	 * see long comment in xpad-toolbar.h. Even if class creators may return GtkWidget*
	 * what is the point of storing them as such. This drastrically reduces readability
	 */

	GtkWidget *textview;
	GtkWidget *scrollbar;
	XpadSearchBar *searchbar;

	/* toolbar stuff — the toolbar and resize grip float over the text as
	   overlay children, each inside a GtkRevealer for the hover animation, so
	   showing them never changes the pad's size. */
	GtkWidget *toolbar;
	GtkWidget *toolbar_revealer;
	GtkWidget *grip_revealer;
	guint toolbar_timeout;
	/* Open menus/popovers (pad menus, toolbar context menu, color picker).
	   While non-zero the autohide toolbar must stay up: popovers anchored
	   inside the toolbar are unmapped with it, and the pointer crossing into
	   a popover's own surface registers as leaving the pad. */
	guint menus_open;

	/* properties window */
	GtkWidget *properties;

	/* preferences/xpad global settings */
	XpadSettings *settings;

	/* context-menu popovers + their backing GMenu models and action group */
	GtkWidget *menu;
	GtkWidget *highlight_menu;
	GMenu *menu_model;
	GMenu *highlight_menu_model;
	GMenu *notes_section;
	GSimpleActionGroup *actions;

	gboolean unsaved_content;
	gboolean unsaved_info;
	gboolean content_load_failed;  /* content file existed but could not be read */

	GdkClipboard *clipboard;

	XpadPadGroup *group;
};

G_DEFINE_TYPE_WITH_PRIVATE (XpadPad, xpad_pad, GTK_TYPE_WINDOW)

enum
{
	CLOSED,
	LAST_SIGNAL
};

enum
{
  PROP_0,
  PROP_GROUP,
  PROP_SETTINGS,
  N_PROPERTIES
};

static GParamSpec *obj_prop[N_PROPERTIES] = { NULL, };
static guint signals[LAST_SIGNAL] = { 0 };

static void xpad_pad_set_property (GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec);
static void xpad_pad_get_property (GObject *object, guint prop_id, GValue *value, GParamSpec *pspec);
static void xpad_pad_constructed (GObject *object);
static void xpad_pad_dispose (GObject *object);
static void xpad_pad_finalize (GObject *object);
static void xpad_pad_load_info (XpadPad *pad, gboolean *show);
static GMenu *menu_get_popup_no_highlight_model (XpadPad *pad);
static GMenu *menu_get_popup_highlight_model (XpadPad *pad);
static void xpad_pad_install_actions (XpadPad *pad);
static void xpad_pad_show (XpadPad *pad);
static void xpad_pad_size_changed (XpadPad *pad);
static gboolean xpad_pad_close_request (XpadPad *pad);
static gboolean xpad_pad_popup_menu (XpadPad *pad);
static void xpad_pad_toggle_edit_lock (XpadPad *pad);
static void menu_popup (XpadPad *pad);
static void menu_popdown (XpadPad *pad);
static void xpad_pad_pressed (GtkGestureClick *gesture, gint n_press, gdouble x, gdouble y, gpointer data);
static void xpad_pad_text_secondary_pressed (GtkGestureClick *gesture, gint n_press, gdouble x, gdouble y, gpointer data);
static void xpad_pad_text_changed (XpadPad *pad, GtkSourceBuffer *buffer);
static void xpad_pad_notify_has_scrollbar (XpadPad *pad);
static void xpad_pad_notify_has_decorations (XpadPad *pad);
static void xpad_pad_notify_has_toolbar (XpadPad *pad);
static void xpad_pad_notify_autohide_toolbar (XpadPad *pad);
static void xpad_pad_hide_toolbar (XpadPad *pad);
static void xpad_pad_show_toolbar (XpadPad *pad);
static void xpad_pad_popup (XpadPad *pad, gdouble x, gdouble y);
static void xpad_pad_spawn (XpadPad *pad);
static void xpad_pad_clear (XpadPad *pad);
static void xpad_pad_search (XpadPad *pad);
static void xpad_pad_undo (XpadPad *pad);
static void xpad_pad_redo (XpadPad *pad);
static void xpad_pad_cut (XpadPad *pad);
static void xpad_pad_copy (XpadPad *pad);
static void xpad_pad_paste (XpadPad *pad);
static void xpad_pad_delete (XpadPad *pad);
static void xpad_pad_open_properties (XpadPad *pad);
static void xpad_pad_open_preferences (XpadPad *pad);
static void xpad_pad_close_all (XpadPad *pad);
static void xpad_pad_sync_title (XpadPad *pad);
static void xpad_pad_enter (GtkEventControllerMotion *controller, gdouble x, gdouble y, gpointer data);
static void xpad_pad_leave (GtkEventControllerMotion *controller, gpointer data);
static void xpad_pad_force_redraw (XpadPad *pad);
static void xpad_pad_connect_surface_heal (XpadPad *pad);
static void xpad_pad_surface_state_changed (GObject *surface, GParamSpec *pspec, gpointer data);

/* Create a new empty pad. */
GtkWidget *
xpad_pad_new (XpadPadGroup *group, XpadSettings *settings)
{
	GtkWidget *pad = GTK_WIDGET (g_object_new (XPAD_TYPE_PAD, "group", group, "settings", settings, NULL));

	/* Classic sticky-note palette: each brand-new pad picks one at random
	   when the random-color setting is on. Existing pads keep whatever their
	   info file says; the choice is persisted through the normal pad-color
	   path (follow-color-style off + explicit colors). */
	gboolean random_color;
	g_object_get (settings, "random-color", &random_color, NULL);
	if (random_color)
	{
		GdkRGBA back;

		gdk_rgba_parse (&back, xpad_sticky_colors[g_random_int_range (0, (gint) xpad_sticky_colors_n)].hex);
		xpad_pad_set_back_color (XPAD_PAD (pad), &back);
	}

	xpad_pad_save_info_delayed (XPAD_PAD (pad));

	return pad;
}

/* Create a new pad based on the provided info-xxxxx file from the config directory and return this pad */
GtkWidget *
xpad_pad_new_with_info (XpadPadGroup *group, XpadSettings *settings, const gchar *info_filename, gboolean *show)
{
	GtkWidget *pad = GTK_WIDGET (g_object_new (XPAD_TYPE_PAD, "group", group, "settings", settings, NULL));

	XPAD_PAD (pad)->priv->infoname = g_strdup (info_filename);
	xpad_pad_load_info (XPAD_PAD (pad), show);
	xpad_pad_load_content (XPAD_PAD (pad));
	/* gtk_window_set_role() was removed in GTK 4 (it was an X11 session-management
	   hint); pad identity is tracked internally via infoname. */

	return pad;
}

/* Create a new pad based on the provided filename from the command line */
GtkWidget *
xpad_pad_new_from_file (XpadPadGroup *group, XpadSettings *settings, const gchar *filename)
{
	GtkWidget *pad = NULL;
	gchar *content;

	content = fio_get_file (filename, CURRENT_WORK_DIR);

	if (!content) {
		gchar *usertext = g_strdup_printf (_("Could not read file %s."), filename);
		xpad_app_error (NULL, usertext, NULL);
		g_free (usertext);
	} else {
		GtkSourceBuffer *buffer;

		pad = xpad_pad_new (group, settings);

		buffer = GTK_SOURCE_BUFFER (gtk_text_view_get_buffer (GTK_TEXT_VIEW (XPAD_PAD (pad)->priv->textview)));

		xpad_text_buffer_freeze_undo (XPAD_TEXT_BUFFER (buffer));

		g_signal_handlers_block_by_func (buffer, xpad_pad_text_changed, pad);
		xpad_text_buffer_set_text_with_tags (XPAD_TEXT_BUFFER (buffer), content ? content : "");
		g_signal_handlers_unblock_by_func (buffer, xpad_pad_text_changed, pad);

		g_free (content);

		xpad_text_buffer_thaw_undo (XPAD_TEXT_BUFFER (buffer));

		xpad_pad_text_changed(XPAD_PAD(pad), buffer);
	}

	return pad;
}

/* Class pad - constructor */
static void
xpad_pad_class_init (XpadPadClass *klass)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

	gobject_class->constructed = xpad_pad_constructed;
	gobject_class->set_property = xpad_pad_set_property;
	gobject_class->get_property = xpad_pad_get_property;
	gobject_class->dispose = xpad_pad_dispose;
	gobject_class->finalize = xpad_pad_finalize;

	signals[CLOSED] =
		g_signal_new ("closed",
						  G_OBJECT_CLASS_TYPE (gobject_class),
						  G_SIGNAL_RUN_FIRST,
						  G_STRUCT_OFFSET (XpadPadClass, closed),
						  NULL, NULL,
						  g_cclosure_marshal_VOID__VOID,
						  G_TYPE_NONE,
						  0);

	/* Properties */
	obj_prop[PROP_GROUP] = g_param_spec_pointer ("group", "Pad group", "Pad group for this pad", G_PARAM_READWRITE | G_PARAM_CONSTRUCT);
	obj_prop[PROP_SETTINGS] = g_param_spec_pointer ("settings", "Xpad settings", "Xpad global settings", G_PARAM_READWRITE | G_PARAM_CONSTRUCT);

	g_object_class_install_properties (gobject_class, N_PROPERTIES, obj_prop);
}

static void
xpad_pad_set_property (GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
	XpadPad *pad = XPAD_PAD (object);

	switch (prop_id)
	{
	case PROP_GROUP:
		pad->priv->group = g_value_get_pointer (value);
		g_object_ref (pad->priv->group);
		if (pad->priv->group)
			xpad_pad_group_add (pad->priv->group, GTK_WIDGET (pad));
		break;

	case PROP_SETTINGS:
		pad->priv->settings = g_value_get_pointer (value);
		g_object_ref (pad->priv->settings);
		break;

	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
xpad_pad_get_property (GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
	XpadPad *pad = XPAD_PAD (object);

	switch (prop_id)
	{
	case PROP_GROUP:
		g_value_set_pointer (value, pad->priv->group);
		break;

	case PROP_SETTINGS:
		g_value_set_pointer (value, pad->priv->settings);
		break;

	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

/* Class pad - initializer */
static void
xpad_pad_init (XpadPad *pad)
{
	pad->priv = xpad_pad_get_instance_private (pad);

	pad->priv->x = 0;
	pad->priv->y = 0;
	pad->priv->infoname = NULL;
	pad->priv->contentname = NULL;
	pad->priv->text_with_search_overlay = NULL;
	pad->priv->textview = NULL;
	pad->priv->scrollbar = NULL;
	pad->priv->searchbar = NULL;
	pad->priv->toolbar = NULL;
	pad->priv->toolbar_revealer = NULL;
	pad->priv->grip_revealer = NULL;
	pad->priv->toolbar_timeout = 0;
	pad->priv->menus_open = 0;
	pad->priv->properties = NULL;
	pad->priv->unsaved_content = FALSE;
	pad->priv->unsaved_info = FALSE;
}

static void xpad_pad_constructed (GObject *object)
{
	XpadPad *pad = XPAD_PAD (object);

	/* Chain up FIRST: GTK 4's gtk_window_constructed() registers the window in
	   the internal toplevel list (gtk_widget_get_native / show all depend on
	   it). Skipping it leaves the pad off that list, which triggers a "window
	   shown after it has been destroyed" warning on every show. */
	G_OBJECT_CLASS (xpad_pad_parent_class)->constructed (object);

	gboolean decorations;

	g_object_get (pad->priv->settings,
			"width", &pad->priv->width,
			"height", &pad->priv->height,
			"autostart-sticky", &pad->priv->sticky, NULL);

	GtkWindow *pad_window = GTK_WINDOW (pad);

	/* textview and scrollbar */
	pad->priv->textview = GTK_WIDGET (XPAD_TEXT_VIEW (xpad_text_view_new (pad->priv->settings, pad)));
	pad->priv->scrollbar = GTK_WIDGET (g_object_new (GTK_TYPE_SCROLLED_WINDOW,
		"hadjustment", NULL,
		"hscrollbar-policy", GTK_POLICY_NEVER,
		"has-frame", FALSE,
		"vadjustment", NULL,
		"vscrollbar-policy", GTK_POLICY_NEVER,
		"child", pad->priv->textview,
		NULL));

	/* searchbar */
	GtkSourceBuffer *buffer = GTK_SOURCE_BUFFER (gtk_text_view_get_buffer (GTK_TEXT_VIEW (pad->priv->textview)));
	pad->priv->searchbar = xpad_search_bar_new (buffer);

	/* overlay with scrollbar (with text) and searchbar */
	pad->priv->text_with_search_overlay = GTK_OVERLAY (gtk_overlay_new ());
	gtk_overlay_add_overlay (pad->priv->text_with_search_overlay, pad->priv->scrollbar);
	gtk_overlay_add_overlay (pad->priv->text_with_search_overlay, GTK_WIDGET (pad->priv->searchbar));

	/* Toolbar — a floating OSD pill overlaid bottom-center on the text and
	   revealed with a fade on hover, instead of a packed bar that resizes the
	   pad. The resize grip gets the same treatment in the bottom corner. */
	pad->priv->toolbar = GTK_WIDGET (xpad_toolbar_new (pad));
	pad->priv->toolbar_revealer = gtk_revealer_new ();
	gtk_revealer_set_transition_type (GTK_REVEALER (pad->priv->toolbar_revealer), GTK_REVEALER_TRANSITION_TYPE_CROSSFADE);
	gtk_revealer_set_transition_duration (GTK_REVEALER (pad->priv->toolbar_revealer), 200);
	gtk_revealer_set_child (GTK_REVEALER (pad->priv->toolbar_revealer), pad->priv->toolbar);
	gtk_widget_set_halign (pad->priv->toolbar_revealer, GTK_ALIGN_CENTER);
	gtk_widget_set_valign (pad->priv->toolbar_revealer, GTK_ALIGN_END);
	gtk_widget_set_margin_bottom (pad->priv->toolbar_revealer, 8);
	gtk_overlay_add_overlay (pad->priv->text_with_search_overlay, pad->priv->toolbar_revealer);
	gtk_overlay_set_clip_overlay (pad->priv->text_with_search_overlay, pad->priv->toolbar_revealer, TRUE);
	/* A collapsed GtkRevealer keeps its allocation; without this, the hidden
	   pill is an invisible click-eating zone over the text. */
	g_object_bind_property (pad->priv->toolbar_revealer, "reveal-child",
		pad->priv->toolbar_revealer, "can-target", G_BINDING_SYNC_CREATE);

	GtkWidget *grip = xpad_grip_tool_item_new ();
	pad->priv->grip_revealer = gtk_revealer_new ();
	gtk_revealer_set_transition_type (GTK_REVEALER (pad->priv->grip_revealer), GTK_REVEALER_TRANSITION_TYPE_CROSSFADE);
	gtk_revealer_set_transition_duration (GTK_REVEALER (pad->priv->grip_revealer), 200);
	gtk_revealer_set_child (GTK_REVEALER (pad->priv->grip_revealer), grip);
	gtk_widget_set_halign (pad->priv->grip_revealer, GTK_ALIGN_END);
	gtk_widget_set_valign (pad->priv->grip_revealer, GTK_ALIGN_END);
	gtk_widget_set_margin_end (pad->priv->grip_revealer, 2);
	gtk_widget_set_margin_bottom (pad->priv->grip_revealer, 2);
	gtk_overlay_add_overlay (pad->priv->text_with_search_overlay, pad->priv->grip_revealer);
	gtk_overlay_set_clip_overlay (pad->priv->text_with_search_overlay, pad->priv->grip_revealer, TRUE);
	g_object_bind_property (pad->priv->grip_revealer, "reveal-child",
		pad->priv->grip_revealer, "can-target", G_BINDING_SYNC_CREATE);

	/* Context-menu actions + accelerators (GtkAccelGroup is gone in GTK 4). */
	xpad_pad_install_actions (pad);
	pad->priv->menu_model = menu_get_popup_no_highlight_model (pad);
	pad->priv->highlight_menu_model = menu_get_popup_highlight_model (pad);
	pad->priv->menu = gtk_popover_menu_new_from_model (G_MENU_MODEL (pad->priv->menu_model));
	pad->priv->highlight_menu = gtk_popover_menu_new_from_model (G_MENU_MODEL (pad->priv->highlight_menu_model));
	gtk_widget_set_parent (pad->priv->menu, GTK_WIDGET (pad));
	gtk_widget_set_parent (pad->priv->highlight_menu, GTK_WIDGET (pad));
	gtk_popover_set_has_arrow (GTK_POPOVER (pad->priv->menu), FALSE);
	gtk_popover_set_has_arrow (GTK_POPOVER (pad->priv->highlight_menu), FALSE);

	GtkEventController *shortcuts = gtk_shortcut_controller_new ();
	gtk_shortcut_controller_set_scope (GTK_SHORTCUT_CONTROLLER (shortcuts), GTK_SHORTCUT_SCOPE_LOCAL);
	gtk_widget_add_controller (GTK_WIDGET (pad), shortcuts);
	#define PAD_ACCEL(keys, action) gtk_shortcut_controller_add_shortcut ( \
		GTK_SHORTCUT_CONTROLLER (shortcuts), \
		gtk_shortcut_new (gtk_shortcut_trigger_parse_string (keys), \
		                  gtk_named_action_new (action)))
	PAD_ACCEL ("<Control>q", "pad.quit");
	PAD_ACCEL ("<Control>n", "pad.new");
	PAD_ACCEL ("<Shift>Delete", "pad.delete");
	PAD_ACCEL ("F5", "pad.reload");
	PAD_ACCEL ("<Control>w", "pad.close");
	PAD_ACCEL ("<Control>z", "pad.undo");
	PAD_ACCEL ("<Control>y", "pad.redo");
	PAD_ACCEL ("<Control>f", "pad.find");
	PAD_ACCEL ("F1", "pad.help");
	PAD_ACCEL ("<Control>b", "pad.bold");
	PAD_ACCEL ("<Control>i", "pad.italic");
	PAD_ACCEL ("<Control>u", "pad.underline");
	/* Keyboard paths that existed only as pointer gestures: the context menu
	   (right-click) and the read-only toggle Preferences has always
	   advertised as CTRL-J without anything implementing it. */
	PAD_ACCEL ("Menu", "pad.menu");
	PAD_ACCEL ("<Shift>F10", "pad.menu");
	PAD_ACCEL ("<Control>j", "pad.toggle-lock");
	#undef PAD_ACCEL

	/* The overlay (text + searchbar + floating toolbar/grip) is the whole
	   window content; nothing is packed below the text anymore. */
	gtk_widget_set_vexpand (GTK_WIDGET (pad->priv->text_with_search_overlay), TRUE);

	g_object_get (pad->priv->settings, "has-decorations", &decorations, NULL);
	gtk_window_set_decorated (pad_window, decorations);
	gtk_window_set_default_size (pad_window, (gint) pad->priv->width, (gint) pad->priv->height);
	/* Pad placement is the compositor's call on Wayland, and the taskbar /
	   pager hints have no Wayland equivalent — both are gone with X11. */

	gtk_window_set_child (pad_window, GTK_WIDGET (pad->priv->text_with_search_overlay));

	xpad_pad_notify_has_scrollbar (pad);

	pad->priv->clipboard = gtk_widget_get_clipboard (GTK_WIDGET (pad));

	xpad_pad_sync_title (pad);

	/* Add CSS style class, so the styling can be overridden by a GTK theme.
	   GTK 4 composites RGBA windows natively, so the old set_visual /
	   app-paintable transparency dance is gone. */
	gtk_widget_add_css_class (GTK_WIDGET (pad), "XpadPad");

	xpad_pad_notify_has_toolbar (pad);

	/* Pointer + click handling via event controllers (GTK 4 removed the
	   button-press-event / enter/leave-notify-event signals). */
	GtkGesture *click = gtk_gesture_click_new ();
	gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click), 0); /* any button */
	g_signal_connect (click, "pressed", G_CALLBACK (xpad_pad_pressed), pad);
	gtk_widget_add_controller (GTK_WIDGET (pad), GTK_EVENT_CONTROLLER (click));

	GtkEventController *motion = gtk_event_controller_motion_new ();
	g_signal_connect (motion, "enter", G_CALLBACK (xpad_pad_enter), pad);
	g_signal_connect (motion, "leave", G_CALLBACK (xpad_pad_leave), pad);
	gtk_widget_add_controller (GTK_WIDGET (pad), motion);

	/* GtkTextView pops its own built-in context menu on right-click, which
	   buries the pad menu (Close, New, Notes, Preferences…) — the text area is
	   nearly the whole pad, so claim secondary presses on it in the capture
	   phase and show the pad menus instead, as the GTK 3 version did. */
	GtkGesture *text_rclick = gtk_gesture_click_new ();
	gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (text_rclick), GDK_BUTTON_SECONDARY);
	gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (text_rclick), GTK_PHASE_CAPTURE);
	g_signal_connect (text_rclick, "pressed", G_CALLBACK (xpad_pad_text_secondary_pressed), pad);
	gtk_widget_add_controller (pad->priv->textview, GTK_EVENT_CONTROLLER (text_rclick));

	/* Render self-heal: once the GdkSurface exists, watch its toplevel state so a
	   blanked pad repaints on minimize/restore / monitor wake (see force_redraw). */
	g_signal_connect (pad, "realize", G_CALLBACK (xpad_pad_connect_surface_heal), NULL);
	g_signal_connect (pad, "close-request", G_CALLBACK (xpad_pad_close_request), NULL);
	/* GTK 4 removed the GtkWidget::show signal; "map" fires when the window is
	   actually put on screen. */
	g_signal_connect (pad, "map", G_CALLBACK (xpad_pad_show), NULL);
	g_signal_connect_swapped (gtk_text_view_get_buffer (GTK_TEXT_VIEW (pad->priv->textview)), "changed", G_CALLBACK (xpad_pad_text_changed), pad);

	g_signal_connect_swapped (pad->priv->settings, "notify::has-decorations", G_CALLBACK (xpad_pad_notify_has_decorations), pad);
	g_signal_connect_swapped (pad->priv->settings, "notify::has-toolbar", G_CALLBACK (xpad_pad_notify_has_toolbar), pad);
	g_signal_connect_swapped (pad->priv->settings, "notify::autohide-toolbar", G_CALLBACK (xpad_pad_notify_autohide_toolbar), pad);
	g_signal_connect_swapped (pad->priv->settings, "notify::has-scrollbar", G_CALLBACK (xpad_pad_notify_has_scrollbar), pad);
	g_signal_connect_swapped (gtk_text_view_get_buffer (GTK_TEXT_VIEW (pad->priv->textview)), "notify::has-selection", G_CALLBACK (xpad_pad_notify_has_selection), pad);
	/* GdkClipboard emits "changed" instead of GtkClipboard's "owner-change". */
	g_signal_connect_swapped (pad->priv->clipboard, "changed", G_CALLBACK (xpad_pad_notify_clipboard_owner_changed), pad);

	g_signal_connect_swapped (pad->priv->toolbar, "activate-new", G_CALLBACK (xpad_pad_spawn), pad);
	g_signal_connect_swapped (pad->priv->toolbar, "activate-clear", G_CALLBACK (xpad_pad_clear), pad);
	g_signal_connect_swapped (pad->priv->toolbar, "activate-close", G_CALLBACK (xpad_pad_close), pad);
	g_signal_connect_swapped (pad->priv->toolbar, "activate-undo", G_CALLBACK (xpad_pad_undo), pad);
	g_signal_connect_swapped (pad->priv->toolbar, "activate-redo", G_CALLBACK (xpad_pad_redo), pad);
	g_signal_connect_swapped (pad->priv->toolbar, "activate-cut", G_CALLBACK (xpad_pad_cut), pad);
	g_signal_connect_swapped (pad->priv->toolbar, "activate-copy", G_CALLBACK (xpad_pad_copy), pad);
	g_signal_connect_swapped (pad->priv->toolbar, "activate-paste", G_CALLBACK (xpad_pad_paste), pad);
	g_signal_connect_swapped (pad->priv->toolbar, "activate-search", G_CALLBACK (xpad_pad_search), pad);
	g_signal_connect_swapped (pad->priv->toolbar, "activate-delete", G_CALLBACK (xpad_pad_delete), pad);
	g_signal_connect_swapped (pad->priv->toolbar, "activate-properties", G_CALLBACK (xpad_pad_open_properties), pad);
	g_signal_connect_swapped (pad->priv->toolbar, "activate-preferences", G_CALLBACK (xpad_pad_open_preferences), pad);
	g_signal_connect_swapped (pad->priv->toolbar, "activate-quit", G_CALLBACK (xpad_pad_close_all), pad);

	g_signal_connect_swapped (pad->priv->toolbar, "popup", G_CALLBACK (menu_popup), pad);
	g_signal_connect_swapped (pad->priv->toolbar, "popdown", G_CALLBACK (menu_popdown), pad);

	/* GtkPopover emits "closed" where GtkMenu emitted "deactivate". */
	g_signal_connect_swapped (pad->priv->menu, "closed", G_CALLBACK (menu_popdown), pad);
	g_signal_connect_swapped (pad->priv->highlight_menu, "closed", G_CALLBACK (menu_popdown), pad);
}

static void
xpad_pad_dispose (GObject *object)
{
	XpadPad *pad = XPAD_PAD (object);

	xpad_pad_remove_accelerator_group (pad);

	/* The autohide toolbar timeout holds a raw (unref'd) pad pointer with no
	   GDestroyNotify. Now that pads actually get finalized (the ref cycles are
	   broken), a pending timeout would fire on freed memory — cancel it. */
	if (pad->priv->toolbar_timeout) {
		g_source_remove (pad->priv->toolbar_timeout);
		pad->priv->toolbar_timeout = 0;
	}

	/* The display clipboard is process-global and shared by every pad (borrowed,
	   not reffed); its "changed" handler carries a raw pad pointer. Disconnect it
	   so a clipboard change after this pad is freed cannot fire on freed memory. */
	if (pad->priv->clipboard) {
		g_signal_handlers_disconnect_by_func (pad->priv->clipboard, (gpointer) xpad_pad_notify_clipboard_owner_changed, pad);
		pad->priv->clipboard = NULL;
	}

	/* Cancel any pending delayed save; the periodic scheduler holds a raw pad
	   pointer that would dangle on the next tick after we are freed. */
	xpad_periodic_remove (pad);

	g_clear_object(&pad->priv->group);

	/* The menus are popovers parented to the pad; unparent before teardown. */
	if (GTK_IS_WIDGET (pad->priv->menu)) {
		gtk_widget_unparent (pad->priv->menu);
		pad->priv->menu = NULL;
	}

	if (GTK_IS_WIDGET (pad->priv->highlight_menu)) {
		gtk_widget_unparent (pad->priv->highlight_menu);
		pad->priv->highlight_menu = NULL;
	}

	g_clear_object (&pad->priv->menu_model);
	g_clear_object (&pad->priv->highlight_menu_model);
	g_clear_object (&pad->priv->notes_section);
	g_clear_object (&pad->priv->actions);

	if (XPAD_IS_PAD_PROPERTIES (pad->priv->properties)) {
		gtk_window_destroy (GTK_WINDOW (pad->priv->properties));
		pad->priv->properties = NULL;
	}

	/* The toolbar handler is disconnected to avoid stray callbacks during
	   teardown; the widget itself is freed as a child of the window. */
	if (XPAD_IS_TOOLBAR (pad->priv->toolbar)) {
		g_signal_handlers_disconnect_matched (pad->priv->toolbar, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, pad);
		pad->priv->toolbar = NULL;
	}

	G_OBJECT_CLASS (xpad_pad_parent_class)->dispose (object);
}

static void
xpad_pad_finalize (GObject *object)
{
	XpadPad *pad = XPAD_PAD (object);

	if (pad->priv->settings) {
		g_signal_handlers_disconnect_matched (pad->priv->settings, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, pad);
		g_clear_object(&pad->priv->settings);
	}

	g_free (pad->priv->infoname);
	g_free (pad->priv->contentname);

	G_OBJECT_CLASS (xpad_pad_parent_class)->finalize (object);
}

static void
xpad_pad_show (XpadPad *pad)
{
	/* No client-side placement: Wayland has no protocol for a window to set
	   its own position, so the compositor decides where the pad lands. The
	   saved x/y are still round-tripped through the info file (see
	   xpad_pad_save_info) purely so a downgrade keeps working. */

	/* Show the pad and set the cursor into the pad */
	gtk_window_present (GTK_WINDOW (pad));
	gtk_widget_grab_focus (GTK_WIDGET (pad->priv->textview));

	/* Save the new visibility status to the disk */
	xpad_pad_save_info_delayed (pad);
}

static gboolean toolbar_timeout (XpadPad *pad)
{
	if (!pad || !pad->priv || !pad->priv->toolbar_timeout)
		return FALSE;

	/* A menu or popover is open (e.g. the color picker, which is anchored
	   inside the toolbar) — hiding now would yank it off screen. */
	if (pad->priv->menus_open) {
		pad->priv->toolbar_timeout = 0;
		return FALSE;
	}

	gboolean has_toolbar, autohide_toolbar;
	g_object_get (pad->priv->settings, "has-toolbar", &has_toolbar, "autohide-toolbar", &autohide_toolbar, NULL);

	if (pad->priv->toolbar_timeout && autohide_toolbar && has_toolbar)
		xpad_pad_hide_toolbar (pad);

	pad->priv->toolbar_timeout = 0;

	return FALSE;
}

static void
xpad_pad_notify_has_decorations (XpadPad *pad)
{
	GtkWindow *pad_window = GTK_WINDOW (pad);
	gboolean decorations;
	g_object_get (pad->priv->settings, "has-decorations", &decorations, NULL);

	/*
	 *  There are two modes of operation:  a normal mode and a 'stealth' mode.
	 *  If decorations are disabled, we also don't show up in the taskbar or pager.
	 *
	 *  GTK 4 applies gtk_window_set_decorated() live, so the old GTK 3
	 *  hide/unrealize/show "reshow with initial size" dance is gone — under
	 *  GTK 4 unrealizing then showing a window triggers a "shown after
	 *  destroyed" warning and leaves it in an inconsistent state.
	 */
	gtk_window_set_decorated (pad_window, decorations);
	gtk_window_set_default_size (pad_window, (gint) pad->priv->width, (gint) pad->priv->height);
}

static void
xpad_pad_notify_has_toolbar (XpadPad *pad)
{
	gboolean has_toolbar, autohide_toolbar;
	g_object_get (pad->priv->settings, "has-toolbar", &has_toolbar, "autohide-toolbar", &autohide_toolbar, NULL);

	if (has_toolbar && !autohide_toolbar)
		xpad_pad_show_toolbar (pad);
	else
		xpad_pad_hide_toolbar (pad);
}

static void
xpad_pad_notify_autohide_toolbar (XpadPad *pad)
{
	gboolean autohide_toolbar;
	g_object_get (pad->priv->settings, "autohide-toolbar", &autohide_toolbar, NULL);

	if (autohide_toolbar)
	{
		/* Likely not to be in pad when turning setting on */
		if (!pad->priv->toolbar_timeout)
			pad->priv->toolbar_timeout = g_timeout_add (1000, (GSourceFunc) toolbar_timeout, pad);
	}
	else
	{
		gboolean has_toolbar;
		g_object_get (pad->priv->settings, "has-toolbar", &has_toolbar, NULL);

		if (has_toolbar)
			xpad_pad_show_toolbar(pad);
	}
}

static void
xpad_pad_notify_has_scrollbar (XpadPad *pad)
{
	gboolean has_scrollbar;
	g_object_get (pad->priv->settings, "has-scrollbar", &has_scrollbar, NULL);

	if (has_scrollbar)
		gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (pad->priv->scrollbar),
			GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	else
	{
		GtkAdjustment *v, *h;

		gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (pad->priv->scrollbar),
			GTK_POLICY_NEVER, GTK_POLICY_NEVER);

		/* now we need to adjust view so that user can see whole pad */
		h = gtk_scrolled_window_get_hadjustment (GTK_SCROLLED_WINDOW (pad->priv->scrollbar));
		v = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (pad->priv->scrollbar));

		gtk_adjustment_set_value (h, 0);
		gtk_adjustment_set_value (v, 0);
	}
}

/* The toolbar and grip float over the text as overlay children, so revealing
   them is a pure animation — none of the GTK 3 era window-resize bookkeeping
   applies anymore. */
static void
xpad_pad_show_toolbar (XpadPad *pad)
{
	gtk_revealer_set_reveal_child (GTK_REVEALER (pad->priv->toolbar_revealer), TRUE);
	gtk_revealer_set_reveal_child (GTK_REVEALER (pad->priv->grip_revealer), TRUE);
}

static void
xpad_pad_hide_toolbar (XpadPad *pad)
{
	gtk_revealer_set_reveal_child (GTK_REVEALER (pad->priv->toolbar_revealer), FALSE);
	gtk_revealer_set_reveal_child (GTK_REVEALER (pad->priv->grip_revealer), FALSE);
}

void
xpad_pad_notify_has_selection (XpadPad *pad)
{
	g_return_if_fail (pad);

	GtkSourceBuffer *buffer;
	buffer = GTK_SOURCE_BUFFER (gtk_text_view_get_buffer (GTK_TEXT_VIEW (pad->priv->textview)));
	gboolean has_selection = gtk_text_buffer_get_has_selection (GTK_TEXT_BUFFER (buffer));

	XpadToolbar *toolbar = XPAD_TOOLBAR (pad->priv->toolbar);
	if (toolbar == NULL)
		return;

	xpad_toolbar_enable_cut_button (toolbar, has_selection);
	xpad_toolbar_enable_copy_button (toolbar, has_selection);
}

/* GdkClipboard has no synchronous gtk_clipboard_wait_is_text_available();
   instead we inspect the currently-advertised content formats. */
static gboolean
xpad_pad_clipboard_has_text (XpadPad *pad)
{
	GdkClipboard *clipboard = gtk_widget_get_clipboard (GTK_WIDGET (pad));
	GdkContentFormats *formats = gdk_clipboard_get_formats (clipboard);
	return gdk_content_formats_contain_gtype (formats, G_TYPE_STRING)
	    || gdk_content_formats_contain_mime_type (formats, "text/plain");
}

void
xpad_pad_notify_clipboard_owner_changed (XpadPad *pad)
{
	g_return_if_fail (pad);

	/* safe cast to toolbar */
	if (XPAD_IS_TOOLBAR (pad->priv->toolbar)) {
		XpadToolbar *toolbar = XPAD_TOOLBAR (pad->priv->toolbar);
		g_return_if_fail (toolbar);

		xpad_toolbar_enable_paste_button (toolbar, xpad_pad_clipboard_has_text (pad));
	}
}

void
xpad_pad_notify_undo_redo_changed (XpadPad *pad)
{
	g_return_if_fail (pad);

	XpadTextBuffer *buffer = NULL;
	buffer = XPAD_TEXT_BUFFER (gtk_text_view_get_buffer (GTK_TEXT_VIEW (pad->priv->textview)));
	g_return_if_fail (buffer);

	XpadToolbar *toolbar = NULL;
	toolbar = XPAD_TOOLBAR (pad->priv->toolbar);
	g_return_if_fail (toolbar);

	xpad_toolbar_enable_undo_button (toolbar, xpad_text_buffer_undo_available (buffer));
	xpad_toolbar_enable_redo_button (toolbar, xpad_text_buffer_redo_available (buffer));
}

/* GTK 4 GtkEventControllerMotion only reports crossings of the widget itself,
   so the old GDK_NOTIFY_INFERIOR / GDK_CROSSING_NORMAL filtering is unnecessary. */
static void
xpad_pad_enter (GtkEventControllerMotion *controller, gdouble x, gdouble y, gpointer data)
{
	(void) controller; (void) x; (void) y;
	XpadPad *pad = XPAD_PAD (data);

	/* Render self-heal: if this pad's surface lost its frame and went blank,
	   the pointer arriving over it is the natural moment to force a repaint. */
	xpad_pad_force_redraw (pad);

	gboolean has_toolbar, autohide_toolbar;
	g_object_get (pad->priv->settings, "has-toolbar", &has_toolbar, "autohide-toolbar", &autohide_toolbar, NULL);

	if (has_toolbar && autohide_toolbar)
	{
		/* Cancel a pending hide-countdown: remove the real GSource, not just the
		   tracking id, so dispose's guard stays accurate and a deleted pad can't
		   leave an armed timer firing on freed memory. */
		if (pad->priv->toolbar_timeout) {
			g_source_remove (pad->priv->toolbar_timeout);
			pad->priv->toolbar_timeout = 0;
		}
		xpad_pad_show_toolbar (pad);
	}
}

/* --- Render self-heal -------------------------------------------------------
 * On NVIDIA + XWayland a single pad's GL render surface can occasionally drop
 * its contents and stay blank while the app keeps running — typically after a
 * KVM/DDC monitor input switch, DPMS off/on, or suspend/resume. GTK 4 will not
 * repaint on its own in that state, so the pad is stuck blank until restart.
 * We force a fresh snapshot on the events that normally follow such a loss:
 * the pointer entering the pad (above) and any toplevel state transition
 * (minimize/restore, monitor wake). gtk_widget_queue_draw is a cheap no-op when
 * nothing is stale, so calling it on these edges is safe. The packaged launcher
 * also pins GSK_RENDERER=cairo, which avoids the GL path entirely; this is the
 * belt-and-suspenders net for any residual case. */
static void
xpad_pad_force_redraw (XpadPad *pad)
{
	gtk_widget_queue_draw (GTK_WIDGET (pad));
	if (pad->priv->textview)
		gtk_widget_queue_draw (pad->priv->textview);
}

static void
xpad_pad_surface_state_changed (GObject *surface, GParamSpec *pspec, gpointer data)
{
	(void) surface; (void) pspec;
	xpad_pad_force_redraw (XPAD_PAD (data));
}

static void
xpad_pad_connect_surface_heal (XpadPad *pad)
{
	GdkSurface *surface = gtk_native_get_surface (GTK_NATIVE (pad));

	/* GdkToplevel exposes a "state" property (GdkToplevelState). The surface is
	   recreated on each realize, so connecting per-realize is correct and the
	   old handler dies with the old surface; g_signal_connect_object ties it to
	   the pad so it is also dropped if the pad is destroyed first. */
	if (surface && GDK_IS_TOPLEVEL (surface))
		g_signal_connect_object (surface, "notify::state",
		                         G_CALLBACK (xpad_pad_surface_state_changed), pad, 0);
}

static void
xpad_pad_leave (GtkEventControllerMotion *controller, gpointer data)
{
	(void) controller;
	XpadPad *pad = XPAD_PAD (data);

	/* Persist geometry when the pointer leaves (configure-event is gone). */
	xpad_pad_size_changed (pad);

	gboolean has_toolbar, autohide_toolbar;
	g_object_get (pad->priv->settings, "has-toolbar", &has_toolbar, "autohide-toolbar", &autohide_toolbar, NULL);

	/* Crossing into an open popover's surface (color picker, context menu)
	   also registers as leaving the pad — don't start the hide countdown
	   while a menu is up; menu_popdown rearms it when the menu closes. */
	if (has_toolbar && autohide_toolbar && !pad->priv->menus_open)
	{
		if (!pad->priv->toolbar_timeout)
			pad->priv->toolbar_timeout = g_timeout_add (1000, (GSourceFunc) toolbar_timeout, pad);
	}
}

static void
xpad_pad_spawn (XpadPad *pad)
{
	GtkWidget *newpad = xpad_pad_new (pad->priv->group, pad->priv->settings);
	gtk_widget_show (newpad);
}

static void
xpad_pad_clear (XpadPad *pad)
{
	GtkTextBuffer *buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (pad->priv->textview));

	GtkTextIter start, end;
	gtk_text_buffer_get_bounds (GTK_TEXT_BUFFER (buffer), &start, &end);

	gtk_text_buffer_begin_user_action (buffer);
	gtk_text_buffer_delete (buffer, &start, &end);
	gtk_text_buffer_end_user_action (buffer);
}

void
xpad_pad_close (XpadPad *pad)
{
	gtk_widget_hide (GTK_WIDGET (pad));

	/*
	 * If no tray and this is the last pad, we don't want to record this
	 * pad as closed, we want to start with just this pad next open.  So
	 * quit before we record.
	 */
	if (!xpad_tray_has_indicator () &&
		 xpad_pad_group_num_visible_pads (pad->priv->group) == 0)
	{
		xpad_app_quit ();
		return;
	}

	if (pad->priv->properties)
		gtk_window_destroy (GTK_WINDOW (pad->priv->properties));

	xpad_pad_save_info_delayed (pad);

	g_signal_emit (pad, signals[CLOSED], 0);
}

void
xpad_pad_search (XpadPad *pad)
{
	xpad_search_bar_show (pad->priv->searchbar);
}

void
xpad_pad_toggle(XpadPad *pad)
{
	 if (gtk_widget_get_visible (GTK_WIDGET(pad)))
		xpad_pad_close (pad);
	 else
		gtk_widget_show (GTK_WIDGET (pad));
}

static gboolean
should_confirm_delete (XpadPad *pad)
{
	GtkSourceBuffer *buffer;
	GtkTextIter s, e;
	gchar *content;
	gboolean confirm;

	g_object_get (pad->priv->settings, "confirm-destroy", &confirm, NULL);
	if (!confirm)
		return FALSE;

	buffer = GTK_SOURCE_BUFFER (gtk_text_view_get_buffer (GTK_TEXT_VIEW (pad->priv->textview)));
	gtk_text_buffer_get_bounds (GTK_TEXT_BUFFER (buffer), &s, &e);
	content = gtk_text_buffer_get_text (GTK_TEXT_BUFFER (buffer), &s, &e, FALSE);

	confirm = strcmp (g_strstrip (content), "") != 0;

	g_free (content);

	return confirm;
}

static void
xpad_pad_delete (XpadPad *pad)
{
	g_return_if_fail (pad);

	if (should_confirm_delete (pad))
	{
		GtkWidget *dialog;
		const gchar *response;

		dialog = xpad_app_alert_dialog (GTK_WINDOW (pad), "dialog-warning", _("Delete this pad?"), _("All text of this pad will be irrevocably lost."));

		if (!dialog)
			return;

		adw_message_dialog_add_responses (ADW_MESSAGE_DIALOG (dialog),
			"cancel", _("_Cancel"),
			"delete", _("_Delete"),
			NULL);
		adw_message_dialog_set_response_appearance (ADW_MESSAGE_DIALOG (dialog), "delete", ADW_RESPONSE_DESTRUCTIVE);
		adw_message_dialog_set_default_response (ADW_MESSAGE_DIALOG (dialog), "cancel");

		response = xpad_app_alert_dialog_run (dialog);

		if (g_strcmp0 (response, "delete") != 0)
			return;
	}

	/* Only once deletion is certain: clear the unsaved flags so the delayed
	   save can't recreate the files we are about to remove. Doing this before
	   the dialog meant Cancel left real edits permanently unsaveable. */
	pad->priv->unsaved_info = FALSE;
	pad->priv->unsaved_content = FALSE;

	/* These two if statements actually erase the pad on the harddisk. */
	if (pad->priv->infoname)
		fio_remove_file (pad->priv->infoname);
	if (pad->priv->contentname)
		fio_remove_file (pad->priv->contentname);

	/* The deletion propagates to the backup mirror; older snapshots still
	   hold the note, which is exactly the safety net we want. */
	xpad_backup_schedule ();

	/* Remove the pad from its group, then destroy it. The group owns a ref on the
	   pad and the pad owns a ref back on the group; GTK 4's gtk_window_destroy()
	   tears the window down but — unlike GTK 3's gtk_widget_destroy() — emits no
	   "destroy" while a ref stands (it fires only on finalize), so the group's
	   "destroy"-signal cleanup never ran and the deleted pad lingered in the group
	   the tray reads. Remove it explicitly here: that is what refreshes the tray,
	   independent of when (or whether) the object is finalized. The temp ref keeps
	   the pad alive across the teardown so nothing is freed mid-call.
	   NB: a separate XpadToolbar->XpadPad ref cycle (xpad-toolbar.c) still keeps
	   the pad object itself alive after this returns — a pre-existing leak tracked
	   for a follow-up; the tray fix here does not depend on that being resolved. */
	XpadPadGroup *group = pad->priv->group;
	g_object_ref (pad);
	if (group)
		xpad_pad_group_remove (group, GTK_WIDGET (pad));
	gtk_window_destroy (GTK_WINDOW (pad));
	g_object_unref (pad);
}

static void
pad_properties_sync_title (XpadPad *pad)
{
	gchar *title;

	if (!pad->priv->properties)
		return;

	title = g_strdup_printf (_("'%s' Layout"), gtk_window_get_title (GTK_WINDOW (pad)));
	gtk_window_set_title (GTK_WINDOW (pad->priv->properties), title);
	g_free (title);
}

static void
pad_properties_destroyed (XpadPad *pad)
{
	if (!pad->priv->properties)
		return;

	g_signal_handlers_disconnect_by_func (pad, (gpointer) pad_properties_sync_title, NULL);
	pad->priv->properties = NULL;
}

static void
prop_notify_font (XpadPad *pad)
{
	XpadPadProperties *prop = XPAD_PAD_PROPERTIES (pad->priv->properties);

	gboolean follow_font_style;
	g_object_get (prop, "follow-font-style", &follow_font_style, NULL);
	g_object_set (XPAD_TEXT_VIEW (pad->priv->textview), "follow-font-style", follow_font_style, NULL);

	if (!follow_font_style)
	{
		gchar *fontname;
		g_object_get (prop, "fontname", &fontname, NULL);

		PangoFontDescription *fontdesc;
		fontdesc = fontname ? pango_font_description_from_string (fontname) : NULL;
		g_free(fontname);

		xpad_text_view_set_font (pad->priv->textview, fontdesc);
		if (fontdesc)
			pango_font_description_free (fontdesc);
	}

	xpad_pad_save_info_delayed (pad);
}

static void
prop_notify_colors (XpadPad *pad)
{
        gboolean follow_color_style;
        XpadPadProperties *prop = XPAD_PAD_PROPERTIES (pad->priv->properties);
        g_object_get (prop, "follow-color-style", &follow_color_style, NULL);
        g_object_set (XPAD_TEXT_VIEW (pad->priv->textview), "follow-color-style", follow_color_style, NULL);

        GdkRGBA *text_color, *back_color;

        if (follow_color_style) {
                /* Use global preference colors */
                g_object_get (pad->priv->settings, "text-color", &text_color, "back-color", &back_color, NULL);
        } else {
                /* Use individual pad colors */
                g_object_get (prop, "text-color", &text_color, "back-color", &back_color, NULL);
        }

        xpad_text_view_set_colors (pad->priv->textview, text_color, back_color);
        gdk_rgba_free (text_color);
        gdk_rgba_free (back_color);

        if (pad->priv->toolbar)
                xpad_toolbar_update_color_swatch (XPAD_TOOLBAR (pad->priv->toolbar));

        xpad_pad_save_info_delayed (pad);
}

/* Give the pad an explicit sticky-note background (toolbar color picker and
   the random-color path). The choice persists through the normal pad-color
   path: follow-color-style off + explicit colors in the info file. */
void
xpad_pad_set_back_color (XpadPad *pad, const GdkRGBA *back)
{
	g_return_if_fail (XPAD_IS_PAD (pad));
	g_return_if_fail (back);

	GdkRGBA text = xpad_sticky_text_color;

	g_object_set (XPAD_TEXT_VIEW (pad->priv->textview), "follow-color-style", FALSE, NULL);
	xpad_text_view_set_colors (pad->priv->textview, &text, (GdkRGBA *) back);

	/* Keep an open properties dialog in sync instead of fighting it. */
	if (pad->priv->properties)
		g_object_set (pad->priv->properties,
			"follow-color-style", FALSE,
			"text-color", &text,
			"back-color", back,
			NULL);

	if (pad->priv->toolbar)
		xpad_toolbar_update_color_swatch (XPAD_TOOLBAR (pad->priv->toolbar));

	xpad_pad_save_info_delayed (pad);
}

/* The background color the pad is actually showing right now — the pad's own
   color, or the global/theme one when the pad follows the default style. */
void
xpad_pad_get_back_color (XpadPad *pad, GdkRGBA *back)
{
	g_return_if_fail (XPAD_IS_PAD (pad));
	g_return_if_fail (back);

	gboolean follow_color_style;
	GdkRGBA *color = NULL;

	g_object_get (XPAD_TEXT_VIEW (pad->priv->textview), "follow-color-style", &follow_color_style, NULL);

	if (follow_color_style)
		g_object_get (pad->priv->settings, "back-color", &color, NULL);
	else
		g_object_get (XPAD_TEXT_VIEW (pad->priv->textview), "back-color", &color, NULL);

	if (color) {
		*back = *color;
		gdk_rgba_free (color);
	} else {
		*back = default_back_color;
	}
}

static void
xpad_pad_open_properties (XpadPad *pad)
{
	if (pad->priv->properties) {
		gtk_window_present (GTK_WINDOW (pad->priv->properties));
		return;
	}

	pad->priv->properties = xpad_pad_properties_new ();

	gtk_window_set_transient_for (GTK_WINDOW (pad->priv->properties), GTK_WINDOW (pad));
	gtk_window_set_resizable (GTK_WINDOW (pad->priv->properties), FALSE);

	g_signal_connect_swapped (pad->priv->properties, "destroy", G_CALLBACK (pad_properties_destroyed), pad);
	g_signal_connect (pad, "notify::title", G_CALLBACK (pad_properties_sync_title), NULL);

	/* GTK 4 removed gtk_style_context_get(GTK_STYLE_PROPERTY_FONT); the resolved
	   font is available from the widget's Pango context. */
	PangoFontDescription *font = pango_context_get_font_description (gtk_widget_get_pango_context (pad->priv->textview));
	gchar *font_name = pango_font_description_to_string (font);

	gboolean follow_font_style, follow_color_style;
	GdkRGBA *text_color = NULL, *back_color = NULL;

	g_object_get (XPAD_TEXT_VIEW (pad->priv->textview),
		"follow-font-style", &follow_font_style,
		"follow-color-style", &follow_color_style,
		"text-color", &text_color,
		"back-color", &back_color,
		NULL);

	g_object_set (G_OBJECT (pad->priv->properties),
		"follow-font-style", follow_font_style,
		"follow-color-style", follow_color_style,
		"text-color", text_color,
		"back-color", back_color,
		"fontname", font_name,
		NULL);

	if (text_color) {
		gdk_rgba_free(text_color);
	}

	if (back_color) {
		gdk_rgba_free(back_color);
	}

	g_free(font_name);

	g_signal_connect_swapped (pad->priv->properties, "notify::follow-font-style", G_CALLBACK (prop_notify_font), pad);
	g_signal_connect_swapped (pad->priv->properties, "notify::follow-color-style", G_CALLBACK (prop_notify_colors), pad);
	g_signal_connect_swapped (pad->priv->properties, "notify::text-color", G_CALLBACK (prop_notify_colors), pad);
	g_signal_connect_swapped (pad->priv->properties, "notify::back-color", G_CALLBACK (prop_notify_colors), pad);
	g_signal_connect_swapped (pad->priv->properties, "notify::fontname", G_CALLBACK (prop_notify_font), pad);

	pad_properties_sync_title (pad);

	gtk_window_present (GTK_WINDOW (pad->priv->properties));
}

static void
xpad_pad_open_preferences (XpadPad *pad)
{
	xpad_preferences_open (pad->priv->settings);
}

static void
xpad_pad_text_changed (XpadPad *pad, GtkSourceBuffer *buffer)
{
	/* A dirty way to silence the compiler for these unused variables. */
	(void) buffer;

	/* set title */
	xpad_pad_sync_title (pad);

	/* record change */
	xpad_pad_save_content_delayed(pad);
}

/* Captures the pad's current geometry and persists it. Replaces the
   configure-event tracking GTK 4 no longer provides; called on pointer-leave
   and on close. On Wayland position cannot be read, so only size is tracked. */
static void
xpad_pad_size_changed (XpadPad *pad)
{
	if (!gtk_widget_get_visible (GTK_WIDGET (pad)))
		return;

	int w = gtk_widget_get_width (GTK_WIDGET (pad));
	int h = gtk_widget_get_height (GTK_WIDGET (pad));

	if (w > 0 && h > 0 && (pad->priv->width != (guint) w || pad->priv->height != (guint) h)) {
		pad->priv->width = (guint) w;
		pad->priv->height = (guint) h;
		xpad_pad_save_info_delayed (pad);
	}
}

/* GtkWindow::close-request replaces the delete-event signal. Returning TRUE
   stops GTK destroying the window so xpad can hide/persist it instead. */
static gboolean
xpad_pad_close_request (XpadPad *pad)
{
	xpad_pad_size_changed (pad);
	xpad_pad_close (pad);

	return TRUE;
}

static gboolean
xpad_pad_popup_menu (XpadPad *pad)
{
	xpad_pad_popup (pad, 10, 10);

	return TRUE;
}

/* CTRL-J, as advertised in Preferences: flip the global read-only setting.
   All pads follow via the settings notify they already subscribe to. */
static void
xpad_pad_toggle_edit_lock (XpadPad *pad)
{
	gboolean lock;
	g_object_get (pad->priv->settings, "edit-lock", &lock, NULL);
	g_object_set (pad->priv->settings, "edit-lock", !lock, NULL);
}

/* Start an interactive move/resize. Shared by the pad itself, the locked-pad
 * text-view drag and the resize grip — all three used to hand-roll the same
 * _NET_WM_MOVERESIZE client message, which only ever worked on X11/XWayland.
 * GDK does it for us on whichever backend is live. */
void
xpad_pad_begin_window_drag (GtkWidget *widget, GtkGestureClick *gesture, gboolean move)
{
	GtkRoot *root = gtk_widget_get_root (widget);
	if (!GTK_IS_WINDOW (root))
		return;

	GdkSurface *surface = gtk_native_get_surface (GTK_NATIVE (root));
	if (!GDK_IS_TOPLEVEL (surface))
		return;

	double x, y;
	if (!gtk_gesture_get_point (GTK_GESTURE (gesture), NULL, &x, &y))
		return;

	/* GDK wants the press point in surface coordinates; the gesture reports it
	   relative to its own widget, and the widget tree can sit at an offset
	   inside the surface (CSD shadows).

	   The sign here is ADD, and it has been queried once already. GTK documents
	   gtk_native_get_surface_transform() as "the translation from surface
	   coordinates into widget coordinates", which reads both ways. Measured on
	   GTK 4.22/mutter 18: a decorated 400x300 window has a 428x329 surface and
	   reports a transform of (14, 12) — i.e. exactly the shadow inset, so
	   widget (0,0) lives at surface (14, 12) and surface = widget + transform.
	   (On X11 the transform is (0, 0), so this is a no-op there either way.) */
	graphene_point_t point;
	if (!gtk_widget_compute_point (widget, GTK_WIDGET (root),
	                               &GRAPHENE_POINT_INIT ((float) x, (float) y), &point))
		return;

	double dx = 0.0, dy = 0.0;
	gtk_native_get_surface_transform (GTK_NATIVE (root), &dx, &dy);

	GdkDevice *device = gtk_gesture_get_device (GTK_GESTURE (gesture));
	int button = (int) gtk_gesture_single_get_current_button (GTK_GESTURE_SINGLE (gesture));
	guint32 time = gtk_event_controller_get_current_event_time (GTK_EVENT_CONTROLLER (gesture));

	if (move)
		gdk_toplevel_begin_move (GDK_TOPLEVEL (surface), device, button,
		                         point.x + dx, point.y + dy, time);
	else
		gdk_toplevel_begin_resize (GDK_TOPLEVEL (surface),
		                           gtk_widget_get_direction (widget) == GTK_TEXT_DIR_RTL
		                             ? GDK_SURFACE_EDGE_SOUTH_WEST
		                             : GDK_SURFACE_EDGE_SOUTH_EAST,
		                           device, button, point.x + dx, point.y + dy, time);

	/* The WM owns the drag now and consumes the button-release, so GTK never
	   sees it. Drop the gesture's implicit grab immediately or the pad freezes
	   to all input waiting for a release that never comes (the v1.4.3 bug).
	   GtkWindowHandle resets its own gesture after begin_move for the same
	   reason, so this stays even though the Xlib grab handling is gone. */
	gtk_event_controller_reset (GTK_EVENT_CONTROLLER (gesture));
}

/* GTK 4 routes button presses through GtkGestureClick (button == 0 → any). */
static void
xpad_pad_pressed (GtkGestureClick *gesture, gint n_press, gdouble x, gdouble y, gpointer data)
{
	(void) n_press;
	XpadPad *pad = XPAD_PAD (data);
	guint button = gtk_gesture_single_get_current_button (GTK_GESTURE_SINGLE (gesture));
	GdkModifierType state = gtk_event_controller_get_current_event_state (GTK_EVENT_CONTROLLER (gesture));
	gboolean ctrl = (state & GDK_CONTROL_MASK) != 0;

	if (button == GDK_BUTTON_PRIMARY && ctrl)
	{
		gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
		xpad_pad_begin_window_drag (GTK_WIDGET (pad), gesture, TRUE);
	}
	else if (button == GDK_BUTTON_SECONDARY)
	{
		if (ctrl)
		{
			gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
			xpad_pad_begin_window_drag (GTK_WIDGET (pad), gesture, FALSE);
		}
		else
		{
			xpad_pad_popup (pad, x, y);
			gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
		}
	}
}

/* Secondary-button press on the text view (capture phase): show the pad menu
   where GtkTextView would otherwise pop its built-in one. Ctrl+right-click
   keeps the interactive-resize behaviour of xpad_pad_pressed. */
static void
xpad_pad_text_secondary_pressed (GtkGestureClick *gesture, gint n_press, gdouble x, gdouble y, gpointer data)
{
	(void) n_press;
	XpadPad *pad = XPAD_PAD (data);
	GtkWidget *textview = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (gesture));
	GdkModifierType state = gtk_event_controller_get_current_event_state (GTK_EVENT_CONTROLLER (gesture));

	gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);

	if (state & GDK_CONTROL_MASK)
	{
		xpad_pad_begin_window_drag (textview, gesture, FALSE);
		return;
	}

	/* The popover is parented to the pad window; translate the press point. */
	graphene_point_t p = GRAPHENE_POINT_INIT ((float) x, (float) y);
	if (!gtk_widget_compute_point (textview, GTK_WIDGET (pad), &p, &p))
		p = GRAPHENE_POINT_INIT ((float) x, (float) y);

	xpad_pad_popup (pad, p.x, p.y);
}

static void
xpad_pad_sync_title (XpadPad *pad)
{
	GtkSourceBuffer *buffer;
	GtkTextIter s, e;
	gchar *content, *end;

	buffer = GTK_SOURCE_BUFFER (gtk_text_view_get_buffer (GTK_TEXT_VIEW (pad->priv->textview)));
	gtk_text_buffer_get_bounds (GTK_TEXT_BUFFER (buffer), &s, &e);
	content = gtk_text_buffer_get_text (GTK_TEXT_BUFFER (buffer), &s, &e, FALSE);
	end = g_utf8_strchr (content, -1, '\n');
	if (end)
		*end = '\0';

	gtk_window_set_title (GTK_WINDOW (pad), g_strstrip (content));

	g_free (content);
}

void
xpad_pad_load_content (XpadPad *pad)
{
	g_return_if_fail (pad);
	g_return_if_fail (pad->priv->contentname);

	gchar *content;
	GtkSourceBuffer *buffer;

	gboolean read_error = FALSE;
	content = fio_get_file_checked (pad->priv->contentname, CONFIG_DIR, &read_error);

	if (read_error) {
		/* The content file exists but could not be read (e.g. a flaky mount). Do
		   NOT load empty content over it or let this pad save — that would destroy
		   the real note. Leave the buffer as it is, flag the pad so saving is
		   blocked, and log. A later Reload (once the file is readable) clears it. */
		g_warning ("Could not read note content '%s'; leaving it unloaded so it is not overwritten. Reload once the file is readable.", pad->priv->contentname);
		pad->priv->content_load_failed = TRUE;
		g_free (content);
		return;
	}
	pad->priv->content_load_failed = FALSE;

	buffer = GTK_SOURCE_BUFFER (gtk_text_view_get_buffer (GTK_TEXT_VIEW (pad->priv->textview)));

	xpad_text_buffer_freeze_undo (XPAD_TEXT_BUFFER (buffer));

	g_signal_handlers_block_by_func (buffer, xpad_pad_text_changed, pad);
	xpad_text_buffer_set_text_with_tags (XPAD_TEXT_BUFFER (buffer), content ? content : "");
	g_signal_handlers_unblock_by_func (buffer, xpad_pad_text_changed, pad);

	g_free (content);
	xpad_text_buffer_thaw_undo (XPAD_TEXT_BUFFER (buffer));

	xpad_pad_sync_title (pad);
	pad->priv->unsaved_content = FALSE;
}

void
xpad_pad_save_content (XpadPad *pad)
{
	g_return_if_fail (pad);

	gchar *content = NULL;
	XpadTextBuffer *buffer;

	if (!pad->priv->unsaved_content) {
		return;
	}

	/* If we never successfully read this note's existing content (a load-time read
	   error left the buffer unloaded), refuse to save — writing the buffer now
	   would overwrite the real file we could not read. Cleared by a successful
	   Reload once the file is readable again. */
	if (pad->priv->content_load_failed) {
		return;
	}

	/* create content file if it doesn't exist yet */
	if (!pad->priv->contentname)
	{
		pad->priv->contentname = fio_unique_name ("content-");
		if (!pad->priv->contentname)
			return;
	}

	if (GTK_IS_TEXT_VIEW(GTK_TEXT_VIEW (pad->priv->textview))) {
		buffer = XPAD_TEXT_BUFFER (gtk_text_view_get_buffer (GTK_TEXT_VIEW (pad->priv->textview)));
		content = xpad_text_buffer_get_text_with_tags (buffer);
	} else {
		g_warning("There is a problem in the program XNote. In function 'xpad_pad_save_content' the variable 'pad->priv->textview' is not of type textview. Please report it at https://github.com/spencernorton/xnote/issues to help improve XNote.");
		return;
	}

	/* Only mark the content clean if the write actually committed; on
	   failure the flag stays set so the next edit / shutdown flush retries
	   instead of silently discarding the text. */
	if (fio_set_file (pad->priv->contentname, content))
		pad->priv->unsaved_content = FALSE;

	g_free (content);

	xpad_backup_schedule ();
}

/* Extract all the metadata of a single pad from its info-xxxxx file and store it in the pad object */
static void
xpad_pad_load_info (XpadPad *pad, gboolean *show)
{
	gboolean locked = FALSE, follow_font = TRUE, follow_color = TRUE, hidden = FALSE;
	gboolean has_toolbar, autohide_toolbar;
	gchar *fontname = NULL, *text_color_string = NULL, *background_color_string = NULL;
	GdkRGBA text_color = {0, 0, 0, 0}, back_color = {0, 0, 0, 0};

	if (!pad->priv->infoname)
		return;

	if (fio_get_values_from_file (pad->priv->infoname,
		"i|width", &pad->priv->width,
		"i|height", &pad->priv->height,
		"i|x", &pad->priv->x,
		"i|y", &pad->priv->y,
		"b|locked", &locked,
		"b|follow_font", &follow_font,
		"b|follow_color", &follow_color,
		"b|sticky", &pad->priv->sticky,
		"b|hidden", &hidden,
		"s|back", &background_color_string,
		"s|text", &text_color_string,
		"s|fontname", &fontname,
		"s|content", &pad->priv->contentname,
		(gchar *) NULL))
		return;

	pad->priv->unsaved_info = FALSE;

	g_object_get (pad->priv->settings, "has-toolbar", &has_toolbar, "autohide-toolbar", &autohide_toolbar, NULL);

	/* Restore size. GTK 4 has no gtk_window_resize(); the default size is
	   honoured before the window is shown. Position is not restored — no
	   Wayland client can place its own window. */
	gtk_window_set_default_size (GTK_WINDOW (pad), (gint) pad->priv->width, (gint) pad->priv->height);

	g_object_set (XPAD_TEXT_VIEW (pad->priv->textview), "follow-font-style", follow_font, "follow-color-style", follow_color, NULL);

	if (locked) {
		g_object_set (XPAD_TEXT_VIEW (pad->priv->textview), "follow-font-style", FALSE, "follow-color-style", FALSE, NULL);
	}

	if (!follow_font)
	{
		PangoFontDescription *font_desc = pango_font_description_from_string (fontname);
		xpad_text_view_set_font(pad->priv->textview, font_desc);
		pango_font_description_free (font_desc);
	}

	if (!follow_color)
	{
		/*
		 * If, for some reason, one of the colors could not be retrieved
		 * (for example due to the migration to the new GdkRGBA colors),
		 * set the color to the default.
		 */
		if (text_color_string == NULL || background_color_string == NULL) {
			text_color = default_text_color;
			back_color = default_back_color;
		}
		else {
			/* If, for some reason, the parsing of the colors fail, set the color to the default. */
			if (!gdk_rgba_parse (&text_color, text_color_string) || !gdk_rgba_parse (&back_color, background_color_string)) {
				text_color = default_text_color;
				back_color = default_back_color;
			}
		}

		/* Set the text and background color for this pad, as stated in its properties file. */
		xpad_text_view_set_colors (pad->priv->textview, &text_color, &back_color);
	}

	if (show)
		*show = !hidden;

	g_free(text_color_string);
	g_free(background_color_string);
	g_free(fontname);
}

void
xpad_pad_save_info (XpadPad *pad)
{
	g_return_if_fail (pad);

	if (!pad->priv->unsaved_info)
		return;

	/* Must create pad info file if it doesn't exist yet */
	if (!pad->priv->infoname) {
		pad->priv->infoname = fio_unique_name ("info-");
		if (!pad->priv->infoname)
			return;
	}

	/* create content file if it doesn't exist yet */
	if (!pad->priv->contentname) {
		pad->priv->contentname = fio_unique_name ("content-");
		if (!pad->priv->contentname)
			return;
	}

	/* The toolbar is an overlay now, so the pad height is always the real
	   content height — no toolbar correction needed. */
	guint height = pad->priv->height;

	/* Resolved font from the widget's Pango context (GTK 4 removed the
	   GTK_STYLE_PROPERTY_FONT style-context query). */
	PangoFontDescription *font = pango_context_get_font_description (gtk_widget_get_pango_context (pad->priv->textview));
	gchar *font_string = pango_font_description_to_string (font);

	gboolean follow_font_style = FALSE, follow_color_style = FALSE;
	GdkRGBA *text_color = NULL, *back_color = NULL;

	g_object_get (XPAD_TEXT_VIEW (pad->priv->textview),
		"follow-font-style", &follow_font_style,
		"follow-color-style", &follow_color_style,
		"text-color", &text_color,
		"back-color", &back_color,
		NULL);

	gchar *text_color_string = gdk_rgba_to_string (text_color);
	gchar *back_color_string = gdk_rgba_to_string (back_color);

	/* As with content: only clear the flag when the write committed, so a
	   failed write is retried rather than silently dropped.

	   x/y are deliberately still read and written even though nothing uses
	   them any more: no Wayland client can set or read its own position, so
	   they can never change, but preserving them byte-for-byte means a
	   downgrade to 2.3.x finds its old coordinates intact instead of
	   scattering every pad to the top-left. Same reasoning as the retired
	   sticky / hide_from_taskbar settings keys. */
	if (fio_set_values_to_file (pad->priv->infoname,
		"i|width", pad->priv->width,
		"i|height", height,
		"i|x", pad->priv->x,
		"i|y", pad->priv->y,
		"b|follow_font", follow_font_style,
		"b|follow_color", follow_color_style,
		"b|sticky", pad->priv->sticky,
		"b|hidden", !gtk_widget_get_visible (GTK_WIDGET(pad)),
		"s|back", back_color_string,
		"s|text", text_color_string,
		"s|fontname", font_string,
		"s|content", pad->priv->contentname,
		(gchar *) NULL) == 0)
		pad->priv->unsaved_info = FALSE;

	g_free (text_color_string);
	g_free (back_color_string);
	g_free (font_string);
	if (text_color)
		gdk_rgba_free (text_color);
	if (back_color)
		gdk_rgba_free (back_color);

	xpad_backup_schedule ();
}

static void
menu_about (XpadPad *pad)
{
	const gchar *artists[] = {"Michael Terry", NULL};
	const gchar *authors[] = {"Arthur Borsboom", "Jeroen Vermeulen", "Michael Terry", "Paul Ivanov", "Sachin Raut", NULL};
	const gchar *comments = _("Sticky notes");
	const gchar *copyright = "\u00A9 2001-2014 Michael Terry\n\u00A9 2013-2024 Arthur Borsboom";
	/* Translators: please translate this as your own name and optionally email
		like so: "Your Name" */
	const gchar *translator_credits = _("translator-credits");
	const gchar *website = "https://github.com/spencernorton/xnote";

	gtk_show_about_dialog (GTK_WINDOW (pad),
		"artists", artists,
		"authors", authors,
		"comments", comments,
		"copyright", copyright,
		"license-type", GTK_LICENSE_GPL_3_0,
		"logo-icon-name", PACKAGE,
		"translator-credits", translator_credits,
		"version", VERSION,
		"website", website,
		NULL);
}

static void
xpad_pad_cut (XpadPad *pad)
{
	gtk_text_buffer_cut_clipboard (
		gtk_text_view_get_buffer (GTK_TEXT_VIEW (pad->priv->textview)),
		gtk_widget_get_clipboard (pad->priv->textview),
		TRUE);
}

static void
xpad_pad_copy (XpadPad *pad)
{
	gtk_text_buffer_copy_clipboard (
		gtk_text_view_get_buffer (GTK_TEXT_VIEW (pad->priv->textview)),
		gtk_widget_get_clipboard (pad->priv->textview));
}

static void
xpad_pad_paste (XpadPad *pad)
{
	gtk_text_buffer_paste_clipboard (
		gtk_text_view_get_buffer (GTK_TEXT_VIEW (pad->priv->textview)),
		gtk_widget_get_clipboard (pad->priv->textview),
		NULL,
		TRUE);
}

static void
xpad_pad_undo (XpadPad *pad)
{
	g_return_if_fail (pad->priv->textview);
	XpadTextBuffer *buffer = NULL;
	buffer = XPAD_TEXT_BUFFER (gtk_text_view_get_buffer (GTK_TEXT_VIEW (pad->priv->textview)));
	g_return_if_fail (buffer);
	xpad_text_buffer_undo (buffer);
}

static void
xpad_pad_redo (XpadPad *pad)
{
	g_return_if_fail (pad->priv->textview);
	XpadTextBuffer *buffer = NULL;
	buffer = XPAD_TEXT_BUFFER (gtk_text_view_get_buffer (GTK_TEXT_VIEW (pad->priv->textview)));
	g_return_if_fail (buffer);
	xpad_text_buffer_redo (buffer);
}

static void
xpad_pad_show_all (XpadPad *pad)
{
	xpad_pad_group_show_all (pad->priv->group);
}

static void
xpad_pad_close_all (XpadPad *pad)
{
	if (!pad->priv->group)
		return;

	/*
	 * The logic is different here depending on whether the tray is open.
	 * If it is open, we just close each pad individually.  If it isn't
	 * open, we do a quit.  This way, when xpad is run again, only the
	 * pads open during the last 'close all' will open again.
	 */
	if (xpad_tray_has_indicator ())
		xpad_pad_group_close_all (pad->priv->group);
	else
		xpad_app_quit ();
}

static void
menu_toggle_tag (XpadPad *pad, const gchar *name)
{
	g_return_if_fail (pad->priv->textview);
	XpadTextBuffer *buffer = NULL;
	buffer = XPAD_TEXT_BUFFER (gtk_text_view_get_buffer (GTK_TEXT_VIEW (pad->priv->textview)));
	xpad_text_buffer_toggle_tag (buffer, name);
	xpad_pad_save_content_delayed(pad);
}

static void
menu_bold (XpadPad *pad)
{
	menu_toggle_tag (pad, "bold");
}

static void
menu_italic (XpadPad *pad)
{
	menu_toggle_tag (pad, "italic");
}

static void
menu_underline (XpadPad *pad)
{
	menu_toggle_tag (pad, "underline");
}

static void
menu_strikethrough (XpadPad *pad)
{
	menu_toggle_tag (pad, "strikethrough");
}

/* Activates the "pad.present" action: bring the n-th pad (by sorted title) to
   the front. The notes submenu items target this action with their index. */
static void
xpad_pad_present_action (GSimpleAction *action, GVariant *param, gpointer data)
{
	(void) action; (void) data;
	gint32 n = g_variant_get_int32 (param);
	GSList *pads = xpad_pad_group_get_pads_sorted_by_title (xpad_app_get_pad_group ());
	XpadPad *target = g_slist_nth_data (pads, (guint) (n - 1));
	if (target)
		gtk_window_present (GTK_WINDOW (target));
	g_slist_free (pads);
}

/* Installs the "pad" action group used by the context-menu GMenu models and the
   keyboard shortcuts. GtkAccelGroup / GtkMenu are gone in GTK 4; menus are now
   declarative GMenu models bound to GActions. */
static void
xpad_pad_install_actions (XpadPad *pad)
{
	static const struct { const char *name; GCallback cb; } simple[] = {
		{ "new",         G_CALLBACK (xpad_pad_spawn) },
		{ "delete",      G_CALLBACK (xpad_pad_delete) },
		{ "reload",      G_CALLBACK (xpad_pad_load_content) },
		{ "close",       G_CALLBACK (xpad_pad_close) },
		{ "undo",        G_CALLBACK (xpad_pad_undo) },
		{ "redo",        G_CALLBACK (xpad_pad_redo) },
		{ "cut",         G_CALLBACK (xpad_pad_cut) },
		{ "copy",        G_CALLBACK (xpad_pad_copy) },
		{ "paste",       G_CALLBACK (xpad_pad_paste) },
		{ "find",        G_CALLBACK (xpad_pad_search) },
		{ "layout",      G_CALLBACK (xpad_pad_open_properties) },
		{ "show-all",    G_CALLBACK (xpad_pad_show_all) },
		{ "close-all",   G_CALLBACK (xpad_pad_close_all) },
		{ "help",        G_CALLBACK (show_help) },
		{ "about",       G_CALLBACK (menu_about) },
		{ "preferences", G_CALLBACK (xpad_pad_open_preferences) },
		{ "quit",        G_CALLBACK (xpad_app_quit) },
		{ "bold",        G_CALLBACK (menu_bold) },
		{ "italic",      G_CALLBACK (menu_italic) },
		{ "underline",   G_CALLBACK (menu_underline) },
		{ "strikethrough", G_CALLBACK (menu_strikethrough) },
		{ "menu",        G_CALLBACK (xpad_pad_popup_menu) },
		{ "toggle-lock", G_CALLBACK (xpad_pad_toggle_edit_lock) },
	};

	pad->priv->actions = g_simple_action_group_new ();

	for (guint i = 0; i < G_N_ELEMENTS (simple); i++) {
		GSimpleAction *a = g_simple_action_new (simple[i].name, NULL);
		/* swapped: the activate callback receives the pad as its first (only)
		   argument; the GVariant parameter is ignored. */
		g_signal_connect_swapped (a, "activate", simple[i].cb, pad);
		g_action_map_add_action (G_ACTION_MAP (pad->priv->actions), G_ACTION (a));
		g_object_unref (a);
	}

	GSimpleAction *present = g_simple_action_new ("present", G_VARIANT_TYPE_INT32);
	g_signal_connect (present, "activate", G_CALLBACK (xpad_pad_present_action), pad);
	g_action_map_add_action (G_ACTION_MAP (pad->priv->actions), G_ACTION (present));
	g_object_unref (present);

	gtk_widget_insert_action_group (GTK_WIDGET (pad), "pad", G_ACTION_GROUP (pad->priv->actions));
}

static GMenu *
menu_get_popup_no_highlight_model (XpadPad *pad)
{
	GMenu *menu = g_menu_new ();

	GMenu *file = g_menu_new ();
	g_menu_append (file, _("_New"), "pad.new");
	g_menu_append (file, _("_Delete"), "pad.delete");
	g_menu_append (file, _("_Reload"), "pad.reload");
	g_menu_append (file, _("_Close"), "pad.close");
	g_menu_append_section (menu, NULL, G_MENU_MODEL (file));
	g_object_unref (file);

	GMenu *edit = g_menu_new ();
	g_menu_append (edit, _("_Undo"), "pad.undo");
	g_menu_append (edit, _("_Redo"), "pad.redo");
	g_menu_append (edit, _("_Paste"), "pad.paste");
	g_menu_append (edit, _("_Find"), "pad.find");
	g_menu_append (edit, _("_Layout"), "pad.layout");
	g_menu_append_submenu (menu, _("_Edit"), G_MENU_MODEL (edit));
	g_object_unref (edit);

	GMenu *notes = g_menu_new ();
	g_menu_append (notes, _("_Show All"), "pad.show-all");
	g_menu_append (notes, _("_Close All"), "pad.close-all");
	/* The live list of pads is rebuilt into this section on each popup. */
	pad->priv->notes_section = g_menu_new ();
	g_menu_append_section (notes, NULL, G_MENU_MODEL (pad->priv->notes_section));
	g_menu_append_submenu (menu, _("_Notes"), G_MENU_MODEL (notes));
	g_object_unref (notes);

	GMenu *help = g_menu_new ();
	g_menu_append (help, _("_Help"), "pad.help");
	g_menu_append (help, _("_About"), "pad.about");
	g_menu_append_submenu (menu, _("_Help"), G_MENU_MODEL (help));
	g_object_unref (help);

	g_menu_append (menu, _("_Preferences"), "pad.preferences");

	return menu;
}

void xpad_pad_append_pad_titles_to_menu (GMenu *section)
{
	/* Get all pads sorted by title. */
	GSList *pads = xpad_pad_group_get_pads_sorted_by_title(xpad_app_get_pad_group ());

	/* Create a fingerprint of the pad titles for usage in the tray logic. */
	GString *pads_fingerprint = g_string_new(NULL);

	gint n = 1;
	for (GSList *l = pads; l; l = l->next, n++) {
		XpadPad *pad = l->data;
		gchar *title = xpad_pad_get_title_for_menu(pad, n);
		g_string_append(pads_fingerprint, title);

		GMenuItem *item = g_menu_item_new (title, NULL);
		g_menu_item_set_action_and_target_value (item, "pad.present", g_variant_new_int32 (n));
		g_menu_append_item (section, item);
		g_object_unref (item);

		g_free (title);
	}

	/* Free the real head — the old loop advanced `pads` itself to NULL, so this
	   used to free NULL and leak the list spine on every no-selection right-click. */
	g_slist_free (pads);

	/* Stash the fingerprint on the section for the tray's change detection. */
	gchar *fingerprint = g_string_free (pads_fingerprint, FALSE);
	g_object_set_data_full (G_OBJECT (section), "pads-fingerprint", fingerprint, g_free);
}

gchar* xpad_pad_get_title_for_menu(XpadPad *pad, gint pad_number) {
	const gchar *full_title = gtk_window_get_title (GTK_WINDOW (pad));
	gchar *tmp_title;

	if (!full_title)
		full_title = "";

	/* Truncate at 20 CHARACTERS, not bytes: a byte cut (g_strndup) through a
	   multi-byte sequence produced invalid UTF-8 that corrupted the tray and
	   Notes menu labels for non-ASCII notes. */
	if (g_utf8_strlen (full_title, -1) > 20) {
		const gchar *end = g_utf8_offset_to_pointer (full_title, 20);
		gchar *cut = g_strndup (full_title, (gsize) (end - full_title));
		tmp_title = g_strconcat (cut, "\342\200\246", NULL); /* U+2026 … */
		g_free (cut);
	} else {
		tmp_title = g_strdup (full_title);
	}

	str_replace_tokens (&tmp_title, '_', "__");
	gchar *title;

	if (pad_number < 10) {
		title = g_strdup_printf ("_%i. %s", pad_number, tmp_title);
	} else {
		title = g_strdup_printf ("%i. %s", pad_number, tmp_title);
	}

	g_free (tmp_title);
	return title;
}

static GMenu *
menu_get_popup_highlight_model (XpadPad *pad)
{
	(void) pad;
	GMenu *menu = g_menu_new ();

	GMenu *clip = g_menu_new ();
	g_menu_append (clip, _("Cu_t"), "pad.cut");
	g_menu_append (clip, _("_Copy"), "pad.copy");
	g_menu_append (clip, _("_Paste"), "pad.paste");
	g_menu_append_section (menu, NULL, G_MENU_MODEL (clip));
	g_object_unref (clip);

	GMenu *fmt = g_menu_new ();
	g_menu_append (fmt, _("_Bold"), "pad.bold");
	g_menu_append (fmt, _("_Italic"), "pad.italic");
	g_menu_append (fmt, _("_Underline"), "pad.underline");
	g_menu_append (fmt, _("_Strikethrough"), "pad.strikethrough");
	g_menu_append_section (menu, NULL, G_MENU_MODEL (fmt));
	g_object_unref (fmt);

	/* Close stays one right-click away even while text is selected. */
	GMenu *win = g_menu_new ();
	g_menu_append (win, _("_Close"), "pad.close");
	g_menu_append_section (menu, NULL, G_MENU_MODEL (win));
	g_object_unref (win);

	return menu;
}

/* Updates action enabled-state + the live notes list before showing the menu. */
static void
menu_prep_popup_no_highlight (XpadPad *pad)
{
	XpadTextBuffer *buffer = XPAD_TEXT_BUFFER (gtk_text_view_get_buffer (GTK_TEXT_VIEW (pad->priv->textview)));
	GActionMap *map = G_ACTION_MAP (pad->priv->actions);

	g_simple_action_set_enabled (G_SIMPLE_ACTION (g_action_map_lookup_action (map, "paste")), xpad_pad_clipboard_has_text (pad));
	g_simple_action_set_enabled (G_SIMPLE_ACTION (g_action_map_lookup_action (map, "undo")), xpad_text_buffer_undo_available (buffer));
	g_simple_action_set_enabled (G_SIMPLE_ACTION (g_action_map_lookup_action (map, "redo")), xpad_text_buffer_redo_available (buffer));

	g_menu_remove_all (pad->priv->notes_section);
	xpad_pad_append_pad_titles_to_menu (pad->priv->notes_section);
}

static void
menu_prep_popup_highlight (XpadPad *pad)
{
	GActionMap *map = G_ACTION_MAP (pad->priv->actions);
	g_simple_action_set_enabled (G_SIMPLE_ACTION (g_action_map_lookup_action (map, "paste")), xpad_pad_clipboard_has_text (pad));
}

static void
menu_popup (XpadPad *pad)
{
	/* Keep the autohide toolbar shown while the menu is open. Cancel any pending
	   hide countdown by removing the real GSource (not just zeroing the tracking
	   id) so a pad deleted while the menu is open can't leave an armed timer
	   firing on freed memory. */
	if (pad->priv->toolbar_timeout) {
		g_source_remove (pad->priv->toolbar_timeout);
		pad->priv->toolbar_timeout = 0;
	}
	pad->priv->menus_open++;
}

static void
menu_popdown (XpadPad *pad)
{
	if (pad->priv->menus_open > 0)
		pad->priv->menus_open--;

	/* Restart the autohide timeout once the last menu is gone; the enter
	   controller cancels it again if the pointer is still over the pad. */
	if (!pad->priv->menus_open && !pad->priv->toolbar_timeout)
		pad->priv->toolbar_timeout = g_timeout_add (1000, (GSourceFunc) toolbar_timeout, pad);
}

static void
xpad_pad_popup (XpadPad *pad, gdouble x, gdouble y)
{
	GtkSourceBuffer *buffer;
	GtkWidget *menu;

	buffer = GTK_SOURCE_BUFFER (gtk_text_view_get_buffer (GTK_TEXT_VIEW (pad->priv->textview)));

	if (gtk_text_buffer_get_selection_bounds (GTK_TEXT_BUFFER (buffer), NULL, NULL))
	{
		menu = pad->priv->highlight_menu;
		menu_prep_popup_highlight (pad);
	}
	else
	{
		menu = pad->priv->menu;
		menu_prep_popup_no_highlight (pad);
	}

	if (!menu)
		return;

	menu_popup (pad);

	gtk_popover_set_pointing_to (GTK_POPOVER (menu),
	                             &(const GdkRectangle){ (int) x, (int) y, 1, 1 });
	gtk_popover_popup (GTK_POPOVER (menu));
}

/* These functions below are used to reduce the amounts of writes, hence improve the performance. */
void xpad_pad_save_content_delayed (XpadPad *pad)
{
	pad->priv->unsaved_content = TRUE;
	xpad_periodic_save_content_delayed (pad);
}

void xpad_pad_save_info_delayed (XpadPad *pad)
{
	pad->priv->unsaved_info = TRUE;
	xpad_periodic_save_info_delayed (pad);
}

/* Save pad without delay, for example on application shutdown. */
void xpad_pad_save_unsaved (XpadPad *pad)
{
	if (pad->priv->unsaved_content)
		xpad_pad_save_content (pad);
	if (pad->priv->unsaved_info)
		xpad_pad_save_info (pad);
}

void xpad_pad_remove_accelerator_group (XpadPad *pad) {
	g_return_if_fail (pad);

	/* GTK 4 has no GtkAccelGroup / per-window accel removal; the pad's
	   shortcuts live on a GtkShortcutController that is torn down with the
	   widget, and the action group is cleared in dispose(). Nothing to do
	   here beyond keeping the call site (used during teardown) valid. */
}
