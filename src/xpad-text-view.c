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

#include <gtk/gtk.h>
#include <gtksourceview/gtksource.h>

#include "constants.h"
#include "xpad-text-view.h"
#include "xpad-text-buffer.h"
#include "xpad-pad.h"
#include "xpad-toolbar.h"

struct XpadTextViewPrivate
{
	gboolean follow_font_style;
	gboolean follow_color_style;
	gulong notify_text_handler;
	gulong notify_back_handler;
	gulong notify_font_handler;
	GtkCssProvider *font_provider;
	GtkCssProvider *color_provider;
	GdkRGBA *text_color;
	GdkRGBA *back_color;
	XpadTextBuffer *buffer;
	XpadSettings *settings;
	XpadPad *pad;
};

G_DEFINE_TYPE_WITH_PRIVATE (XpadTextView, xpad_text_view, GTK_SOURCE_TYPE_VIEW)

static void xpad_text_view_set_property (GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec);
static void xpad_text_view_get_property (GObject *object, guint prop_id, GValue *value, GParamSpec *pspec);
static void xpad_text_view_constructed (GObject *object);
static void xpad_text_view_dispose (GObject *object);
static void xpad_text_view_finalize (GObject *object);
static void xpad_text_view_realize (XpadTextView *widget);
static void xpad_text_view_pressed (GtkGestureClick *gesture, gint n_press, gdouble x, gdouble y, gpointer data);
static void xpad_text_view_focus_leave (GtkEventControllerFocus *controller, gpointer data);
static void xpad_text_view_notify_edit_lock (XpadTextView *view);
static void xpad_text_view_notify_editable (XpadTextView *view);
static void xpad_text_view_notify_fontname (XpadTextView *view);
static void xpad_text_view_notify_colors (XpadTextView *view);
static void xpad_text_view_notify_line_numbering (XpadTextView *view);

enum
{
	PROP_0,
	PROP_SETTINGS,
	PROP_PAD,
	PROP_FOLLOW_FONT_STYLE,
	PROP_FOLLOW_COLOR_STYLE,
	PROP_TEXT_COLOR,
	PROP_BACK_COLOR,
	N_PROPERTIES
};

static GParamSpec *obj_prop[N_PROPERTIES] = { NULL, };

GtkWidget *
xpad_text_view_new (XpadSettings *settings, XpadPad *pad)
{
	return GTK_WIDGET (g_object_new (XPAD_TYPE_TEXT_VIEW,
		"settings", settings,
		"pad", pad,
		NULL));
}

static void
xpad_text_view_class_init (XpadTextViewClass *klass)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

	gobject_class->constructed = xpad_text_view_constructed;
	gobject_class->dispose = xpad_text_view_dispose;
	gobject_class->finalize = xpad_text_view_finalize;
	gobject_class->set_property = xpad_text_view_set_property;
	gobject_class->get_property = xpad_text_view_get_property;

	obj_prop[PROP_SETTINGS] = g_param_spec_pointer ("settings", "Xpad settings", "Xpad global settings", G_PARAM_READWRITE | G_PARAM_CONSTRUCT);
	obj_prop[PROP_PAD] = g_param_spec_pointer ("pad", "Pad", "Pad connected to this textview", G_PARAM_READWRITE | G_PARAM_CONSTRUCT);
	obj_prop[PROP_FOLLOW_FONT_STYLE] = g_param_spec_boolean ("follow-font-style", "Follow font style", "Whether to use the default XNote font style", TRUE, G_PARAM_READWRITE | G_PARAM_CONSTRUCT);
	obj_prop[PROP_FOLLOW_COLOR_STYLE] = g_param_spec_boolean ("follow-color-style", "Follow color style", "Whether to use the default XNote color style", TRUE, G_PARAM_READWRITE | G_PARAM_CONSTRUCT);
	obj_prop[PROP_TEXT_COLOR] = g_param_spec_boxed ("text-color", "Text and caret color", "The color for the text and the cursor", GDK_TYPE_RGBA, G_PARAM_READWRITE | G_PARAM_CONSTRUCT);
	obj_prop[PROP_BACK_COLOR] = g_param_spec_boxed ("back-color", "Background color", "The color for the background", GDK_TYPE_RGBA, G_PARAM_READWRITE | G_PARAM_CONSTRUCT);

	g_object_class_install_properties (gobject_class, N_PROPERTIES, obj_prop);
}

static void
xpad_text_view_init (XpadTextView *view) {
	view->priv = xpad_text_view_get_instance_private (view);

	view->priv->follow_font_style = TRUE;
	view->priv->follow_color_style = TRUE;
	view->priv->text_color = gdk_rgba_copy(&default_text_color);
	view->priv->back_color = gdk_rgba_copy(&default_back_color);
}

static void
xpad_text_view_constructed (GObject *object)
{
	XpadTextView *view = XPAD_TEXT_VIEW (object);

	view->priv->buffer = xpad_text_buffer_new (view->priv->pad);

	gtk_text_view_set_buffer (GTK_TEXT_VIEW (view), GTK_TEXT_BUFFER (view->priv->buffer));
	gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (view), GTK_WRAP_WORD);
	gtk_text_view_set_top_margin (GTK_TEXT_VIEW (view), 5);
	gtk_text_view_set_bottom_margin (GTK_TEXT_VIEW (view), 5);
	gtk_text_view_set_left_margin (GTK_TEXT_VIEW (view), 5);
	gtk_text_view_set_right_margin (GTK_TEXT_VIEW (view), 5);

	gchar *widget_name = g_strdup_printf ("%p", (void *) view);
	gtk_widget_set_name (GTK_WIDGET (view), widget_name);
	g_free (widget_name);
	xpad_text_view_notify_line_numbering(view);
	xpad_text_view_notify_colors(view);

	/* Add CSS style class, so the styling can be overridden by a GTK theme */
	gtk_widget_add_css_class (GTK_WIDGET (view), "XpadTextView");

	/* Signals. GTK 4 routes pointer/focus through event controllers rather
	   than the old button-press-event / focus-out-event signals. */
	GtkGesture *click = gtk_gesture_click_new ();
	gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click), GDK_BUTTON_PRIMARY);
	g_signal_connect (click, "pressed", G_CALLBACK (xpad_text_view_pressed), view->priv->settings);
	gtk_widget_add_controller (GTK_WIDGET (view), GTK_EVENT_CONTROLLER (click));

	GtkEventController *focus = gtk_event_controller_focus_new ();
	g_signal_connect (focus, "leave", G_CALLBACK (xpad_text_view_focus_leave), view->priv->settings);
	gtk_widget_add_controller (GTK_WIDGET (view), focus);

	g_signal_connect (view, "realize", G_CALLBACK (xpad_text_view_realize), NULL);
	g_signal_connect (view, "notify::editable", G_CALLBACK (xpad_text_view_notify_editable), NULL);
	g_signal_connect_swapped (view->priv->settings, "notify::edit-lock", G_CALLBACK (xpad_text_view_notify_edit_lock), view);
	g_signal_connect_swapped (view->priv->settings, "notify::line-numbering", G_CALLBACK (xpad_text_view_notify_line_numbering), view);

	view->priv->notify_font_handler = g_signal_connect_swapped (view->priv->settings, "notify::fontname", G_CALLBACK (xpad_text_view_notify_fontname), view);
	view->priv->notify_text_handler = g_signal_connect_swapped (view->priv->settings, "notify::text-color", G_CALLBACK (xpad_text_view_notify_colors), view);
	view->priv->notify_back_handler = g_signal_connect_swapped (view->priv->settings, "notify::back-color", G_CALLBACK (xpad_text_view_notify_colors), view);

	g_signal_handler_block (view->priv->settings, view->priv->notify_font_handler);
}

static void
xpad_text_view_dispose (GObject *object)
{
	XpadTextView *view = XPAD_TEXT_VIEW (object);

	g_clear_object (&view->priv->buffer);
	view->priv->pad = NULL;  /* non-owning back-reference; nothing to unref */
	/* Disconnect our notify:: handlers on the settings singleton BEFORE releasing
	   our ref to it. This must happen in dispose while settings is still non-NULL;
	   the old code did it in finalize, but dispose had already cleared settings, so
	   the disconnect never ran and the handlers fired on the freed view whenever a
	   Preferences change (font/color/edit-lock/line-numbering) was made. */
	if (view->priv->settings)
		g_signal_handlers_disconnect_matched (view->priv->settings, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, view);
	g_clear_object (&view->priv->settings);
	g_clear_object (&view->priv->font_provider);
	g_clear_object (&view->priv->color_provider);

	if (view->priv->text_color) {
		gdk_rgba_free(view->priv->text_color);
		view->priv->text_color = NULL;
	}

	if (view->priv->back_color) {
		gdk_rgba_free(view->priv->back_color);
		view->priv->back_color = NULL;
	}

	G_OBJECT_CLASS (xpad_text_view_parent_class)->dispose (object);
}

static void
xpad_text_view_finalize (GObject *object)
{
	/* The settings-singleton handlers are disconnected in dispose (before the
	   settings ref is released), so there is nothing settings-related to do here. */
	G_OBJECT_CLASS (xpad_text_view_parent_class)->finalize (object);
}

static void
xpad_text_view_realize (XpadTextView *view)
{
	gboolean edit_lock;
	g_object_get (view->priv->settings, "edit-lock", &edit_lock, NULL);
	gtk_text_view_set_editable (GTK_TEXT_VIEW (view), !edit_lock);
}

static void
xpad_text_view_set_property (GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
	XpadTextView *view = XPAD_TEXT_VIEW (object);

	switch (prop_id)
	{
	case PROP_SETTINGS:
		view->priv->settings = g_value_get_pointer (value);
		g_object_ref (view->priv->settings);
		break;

	case PROP_PAD:
		/* Non-owning: the pad owns this text view (its child) and outlives it;
		   a ref here would be a finalize-blocking cycle. */
		view->priv->pad = g_value_get_pointer (value);
		break;

	case PROP_FOLLOW_FONT_STYLE:
		view->priv->follow_font_style = g_value_get_boolean (value);
		if (view->priv->follow_font_style) {
			xpad_text_view_notify_fontname (view);
			if (view->priv->notify_font_handler != 0) {
				g_signal_handler_unblock (view->priv->settings, view->priv->notify_font_handler);
			}
		}
		else
			g_signal_handler_block (view->priv->settings, view->priv->notify_font_handler);
		break;

	case PROP_FOLLOW_COLOR_STYLE:
		view->priv->follow_color_style = g_value_get_boolean (value);
		break;

	case PROP_TEXT_COLOR:
		if (view->priv->text_color) {
			gdk_rgba_free(view->priv->text_color);
		}

		GdkRGBA *text_color = g_value_get_boxed(value);
		view->priv->text_color = text_color ? gdk_rgba_copy(text_color) : gdk_rgba_copy(&default_text_color);
 		break;

	case PROP_BACK_COLOR:
		if (view->priv->back_color) {
	                gdk_rgba_free(view->priv->back_color);
		}

		GdkRGBA *back_color = g_value_get_boxed(value);
		view->priv->back_color = back_color ? gdk_rgba_copy(back_color) : gdk_rgba_copy(&default_back_color);
		break;

	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
xpad_text_view_get_property (GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
	XpadTextView *view = XPAD_TEXT_VIEW (object);

	switch (prop_id)
	{
	case PROP_SETTINGS:
		g_value_set_pointer (value, view->priv->settings);
		break;

	case PROP_PAD:
		g_value_set_pointer (value, view->priv->pad);
		break;

	case PROP_FOLLOW_FONT_STYLE:
		g_value_set_boolean (value, view->priv->follow_font_style);
		break;

	case PROP_FOLLOW_COLOR_STYLE:
		g_value_set_boolean (value, view->priv->follow_color_style);
		break;

	case PROP_TEXT_COLOR:
		g_value_set_boxed(value, view->priv->text_color);
		break;

	case PROP_BACK_COLOR:
		g_value_set_boxed(value, view->priv->back_color);
		break;

	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
xpad_text_view_focus_leave (GtkEventControllerFocus *controller, gpointer data)
{
	XpadSettings *settings = data;
	GtkWidget *widget = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (controller));

	gboolean edit_lock;
	g_object_get (settings, "edit-lock", &edit_lock, NULL);

	if (edit_lock)
		gtk_text_view_set_editable (GTK_TEXT_VIEW (widget), FALSE);
}

static void
xpad_text_view_pressed (GtkGestureClick *gesture, gint n_press, gdouble x, gdouble y, gpointer data)
{
	(void) x;
	(void) y;

	XpadSettings *settings = data;
	GtkWidget *widget = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (gesture));

	gboolean edit_lock;
	g_object_get (settings, "edit-lock", &edit_lock, NULL);

	if (edit_lock && !gtk_text_view_get_editable (GTK_TEXT_VIEW (widget)))
	{
		if (n_press >= 2)
		{
			/* Double-click on a locked pad unlocks it for editing. */
			gtk_text_view_set_editable (GTK_TEXT_VIEW (widget), TRUE);
			gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
		}
		else
		{
			/* Single-click drags the whole pad. */
			gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
			xpad_pad_begin_window_drag (widget, gesture, TRUE);
		}
	}
}

static void
xpad_text_view_notify_edit_lock (XpadTextView *view)
{
	/* chances are good that they don't have the text view focused while it changed, so make non-editable if edit lock turned on */
	gboolean edit_lock;
	g_object_get (view->priv->settings, "edit-lock", &edit_lock, NULL);
	gtk_text_view_set_editable (GTK_TEXT_VIEW (view), !edit_lock);
}

static void
xpad_text_view_notify_editable (XpadTextView *view)
{
	gboolean editable;
	GtkSourceView *view_tv = GTK_SOURCE_VIEW (view);

	editable = gtk_text_view_get_editable (GTK_TEXT_VIEW (view_tv));
	gtk_text_view_set_cursor_visible (GTK_TEXT_VIEW (view_tv), editable);

	/* GTK 4 has no per-text-window GdkWindow; set the widget cursor by name.
	   An editable pad shows the I-beam text cursor, a locked one the default. */
	gtk_widget_set_cursor_from_name (GTK_WIDGET (view), editable ? "text" : "default");
}

static void
xpad_text_view_notify_fontname (XpadTextView *view)
{
	gchar *font;
	g_object_get (view->priv->settings, "fontname", &font, NULL);

	PangoFontDescription *fontdesc = font ? pango_font_description_from_string (font) : NULL;
	xpad_text_view_set_font (GTK_WIDGET (view), fontdesc);
	if (fontdesc)
		pango_font_description_free (fontdesc);

	g_free (font);
}

/* Update the colors of the textview */
static void
xpad_text_view_notify_colors (XpadTextView *view)
{
	GdkRGBA *text_color, *back_color;

	if (view->priv->follow_color_style) {
		/* Use global colors */
		g_object_get (view->priv->settings, "text-color", &text_color, "back-color", &back_color, NULL);
	} else {
		/* Use individual colors */
		g_object_get (view, "text-color", &text_color, "back-color", &back_color, NULL);
	}

	GtkWidget *view_widget = GTK_WIDGET (view);
	xpad_text_view_set_colors(view_widget, text_color, back_color);
	gdk_rgba_free (text_color);
	gdk_rgba_free (back_color);
}

/* Set the foreground and background color of the visible part of the pad, which is the text view */
void
xpad_text_view_set_colors (GtkWidget *view, GdkRGBA *text_color, GdkRGBA *back_color) {
	g_object_set(view, "text-color", text_color, "back-color", back_color, NULL);

	gchar *text_color_string = text_color ? gdk_rgba_to_string (text_color) : g_strdup("@theme_fg_color");
	gchar *back_color_string = back_color ? gdk_rgba_to_string (back_color) : g_strdup("@theme_bg_color");

	gchar *cssStyling = g_strconcat(
			"textview, textview text {caret-color: ", text_color_string,
			"; color: ", text_color_string,
			"; background-color: ", back_color_string,
			";}\n", NULL);

	GtkStyleContext *context = gtk_widget_get_style_context (view);
	if (XPAD_TEXT_VIEW (view)->priv->color_provider) {
		gtk_style_context_remove_provider (context, GTK_STYLE_PROVIDER (XPAD_TEXT_VIEW (view)->priv->color_provider));
		g_clear_object (&XPAD_TEXT_VIEW (view)->priv->color_provider);
	}
	GtkCssProvider *provider = gtk_css_provider_new ();
	gtk_css_provider_load_from_data (provider, cssStyling, -1);
	gtk_style_context_add_provider (context, GTK_STYLE_PROVIDER (provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	XPAD_TEXT_VIEW (view)->priv->color_provider = provider;

	g_free(cssStyling);
	g_free(text_color_string);
	g_free(back_color_string);
}

/* Set the font of the pad, which is in the text view */
void
xpad_text_view_set_font (GtkWidget *view, PangoFontDescription *desc) {
	GtkStyleContext *context = gtk_widget_get_style_context (view);

	if (desc == NULL && XPAD_TEXT_VIEW (view)->priv->font_provider) {
		/* Remove font provider */
		gtk_style_context_remove_provider (context, GTK_STYLE_PROVIDER (XPAD_TEXT_VIEW (view)->priv->font_provider));
		g_clear_object (&XPAD_TEXT_VIEW (view)->priv->font_provider);
	} else {
		/* Add/replace font provider */
		gchar *font_description;
		gchar *cssStyling;

		font_description = pango_font_description_to_css(desc);
		cssStyling = g_strconcat("textview, textview text ", font_description, "\n", NULL);
		g_free (font_description);

		if (XPAD_TEXT_VIEW (view)->priv->font_provider) {
			gtk_style_context_remove_provider (context, GTK_STYLE_PROVIDER (XPAD_TEXT_VIEW (view)->priv->font_provider));
			g_clear_object (&XPAD_TEXT_VIEW (view)->priv->font_provider);
		}

		GtkCssProvider *provider = gtk_css_provider_new ();
		gtk_css_provider_load_from_data (provider, cssStyling, -1);
		/* APPLICATION priority, not SETTINGS: on a real desktop the session's
		   UI font (gtk-font-name / theme) is registered at PRIORITY_SETTINGS,
		   so a font override added at the same priority ties and the desktop
		   font wins — the user's font choice has "no effect". The colour
		   provider hit the identical problem and was already moved up to
		   PRIORITY_APPLICATION (commit a286b66a5); the font provider was left
		   behind. Keep the two in lockstep so the pad font actually applies. */
		gtk_style_context_add_provider (context, GTK_STYLE_PROVIDER (provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
		XPAD_TEXT_VIEW (view)->priv->font_provider = provider;

		g_free (cssStyling);
	}
}

static void
xpad_text_view_notify_line_numbering (XpadTextView *view)
{
	gboolean line_numbering;
	g_object_get (view->priv->settings, "line-numbering", &line_numbering, NULL);
	gtk_source_view_set_show_line_numbers (GTK_SOURCE_VIEW (view), line_numbering);
}
