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

#include <glib/gi18n.h>
#include <gtk/gtk.h>

#include "xpad-pad-properties.h"

struct XpadPadPropertiesPrivate
{
	GtkWidget *fontcheck;
	GtkWidget *colorcheck;
	GtkWidget *colorbox;

	GtkWidget *textbutton;
	GdkRGBA texttmp;

	GtkWidget *backbutton;
	GdkRGBA backtmp;

	GtkWidget *fontbutton;
};

G_DEFINE_TYPE_WITH_PRIVATE (XpadPadProperties, xpad_pad_properties, GTK_TYPE_DIALOG)

static void xpad_pad_properties_set_property (GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec);
static void xpad_pad_properties_get_property (GObject *object, guint prop_id, GValue *value, GParamSpec *pspec);
static void xpad_pad_properties_response (GtkDialog *dialog, gint response);
static void change_color_check (GtkCheckButton *button, XpadPadProperties *prop);
static void change_font_check (GtkCheckButton *button, XpadPadProperties *prop);
static void change_text_color (XpadPadProperties *prop);
static void change_back_color (XpadPadProperties *prop);
static void change_font_face (XpadPadProperties *prop);

enum
{
	PROP_0,
	PROP_FOLLOW_FONT_STYLE,
	PROP_FOLLOW_COLOR_STYLE,
	PROP_BACK_COLOR,
	PROP_TEXT_COLOR,
	PROP_FONTNAME,
	N_PROPERTIES
};

static GParamSpec *obj_prop[N_PROPERTIES] = { NULL, };

GtkWidget *
xpad_pad_properties_new (void)
{
	return g_object_new (XPAD_TYPE_PAD_PROPERTIES, NULL);
}

static void
xpad_pad_properties_class_init (XpadPadPropertiesClass *klass)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

	gobject_class->set_property = xpad_pad_properties_set_property;
	gobject_class->get_property = xpad_pad_properties_get_property;

	obj_prop[PROP_FOLLOW_FONT_STYLE] = g_param_spec_boolean ("follow-font-style", "Follow font style", "Whether to use the default XNote font style", TRUE, G_PARAM_READWRITE);
	obj_prop[PROP_FOLLOW_COLOR_STYLE] = g_param_spec_boolean ("follow-color-style", "Follow color style", "Whether to use the default XNote color style", TRUE, G_PARAM_READWRITE);
	obj_prop[PROP_TEXT_COLOR] = g_param_spec_boxed ("text-color", "Text color", "The color of text in this pad", GDK_TYPE_RGBA, G_PARAM_READWRITE);
	obj_prop[PROP_BACK_COLOR] = g_param_spec_boxed ("back-color", "Back color", "The color of the background in this pad", GDK_TYPE_RGBA, G_PARAM_READWRITE);
	obj_prop[PROP_FONTNAME] = g_param_spec_string ("fontname", "Font name", "The name of the font for this pad", NULL, G_PARAM_READWRITE);

	g_object_class_install_properties (gobject_class, N_PROPERTIES, obj_prop);
}

static void
xpad_pad_properties_init (XpadPadProperties *prop)
{
	GtkBox *hbox, *font_hbox, *vbox, *appearance_vbox;
	GtkWidget *font_radio, *color_radio, *label;
	GtkSizeGroup *size_group_labels = gtk_size_group_new (GTK_SIZE_GROUP_HORIZONTAL);

	prop->priv = xpad_pad_properties_get_instance_private (prop);

	appearance_vbox = GTK_BOX (gtk_box_new (GTK_ORIENTATION_VERTICAL, 18));
	gtk_box_set_homogeneous (appearance_vbox, FALSE);
	gtk_widget_set_margin_bottom (GTK_WIDGET (appearance_vbox), 12);

	gchar *appearance_markup = g_strconcat ("<b>", _("Appearance"), "</b>", NULL);
	label = gtk_label_new (appearance_markup);
	g_free (appearance_markup);
	gtk_label_set_use_markup (GTK_LABEL (label), TRUE);
	gtk_box_append (appearance_vbox, label);

	prop->priv->fontbutton = gtk_font_button_new ();
	prop->priv->textbutton = gtk_color_button_new ();
	prop->priv->backbutton = gtk_color_button_new ();

	/* GTK 4 removed GtkRadioButton; a grouped set of GtkCheckButtons behaves as
	   radio buttons (only one active at a time). */
	font_radio = gtk_check_button_new_with_mnemonic (_("Use font from XNote preferences"));
	prop->priv->fontcheck = gtk_check_button_new_with_mnemonic (_("Use this font:"));
	gtk_check_button_set_group (GTK_CHECK_BUTTON (prop->priv->fontcheck), GTK_CHECK_BUTTON (font_radio));
	color_radio = gtk_check_button_new_with_mnemonic (_("Use colors from XNote preferences"));
	prop->priv->colorcheck = gtk_check_button_new_with_mnemonic (_("Use these colors:"));
	gtk_check_button_set_group (GTK_CHECK_BUTTON (prop->priv->colorcheck), GTK_CHECK_BUTTON (color_radio));

	font_hbox = GTK_BOX (gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6));

	gtk_box_append (font_hbox, prop->priv->fontcheck);
	gtk_widget_set_hexpand (prop->priv->fontbutton, TRUE);
	gtk_box_append (font_hbox, prop->priv->fontbutton);

	prop->priv->colorbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);

	hbox = GTK_BOX (gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12));
	label = gtk_label_new_with_mnemonic (_("Foreground:"));
	gtk_label_set_xalign (GTK_LABEL (label), 0.0);
	gtk_size_group_add_widget (size_group_labels, label);
	gtk_box_append (hbox, label);
	gtk_widget_set_hexpand (prop->priv->textbutton, TRUE);
	gtk_box_append (hbox, prop->priv->textbutton);
	gtk_box_append (GTK_BOX (prop->priv->colorbox), GTK_WIDGET (hbox));

	hbox = GTK_BOX (gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12));
	label = gtk_label_new_with_mnemonic (_("Background:"));
	gtk_label_set_xalign (GTK_LABEL (label), 0.0);
	gtk_size_group_add_widget (size_group_labels, label);
	gtk_box_append (hbox, label);
	gtk_widget_set_hexpand (prop->priv->backbutton, TRUE);
	gtk_box_append (hbox, prop->priv->backbutton);
	gtk_box_append (GTK_BOX (prop->priv->colorbox), GTK_WIDGET (hbox));

	gtk_dialog_add_button (GTK_DIALOG (prop), _("_Close"), GTK_RESPONSE_CLOSE);
	gtk_dialog_set_default_response (GTK_DIALOG (prop), GTK_RESPONSE_CLOSE);
	g_signal_connect (prop, "response", G_CALLBACK (xpad_pad_properties_response), NULL);

	gtk_color_chooser_set_use_alpha (GTK_COLOR_CHOOSER (prop->priv->textbutton), FALSE);
	gtk_color_chooser_set_use_alpha (GTK_COLOR_CHOOSER (prop->priv->backbutton), TRUE);

	gtk_color_button_set_title (GTK_COLOR_BUTTON (prop->priv->textbutton), _("Set Foreground Color"));
	gtk_color_button_set_title (GTK_COLOR_BUTTON (prop->priv->backbutton), _("Set Background Color"));
	gtk_font_button_set_title (GTK_FONT_BUTTON (prop->priv->fontbutton), _("Set Font"));

	vbox = GTK_BOX (gtk_box_new (GTK_ORIENTATION_VERTICAL, 6));
	hbox = GTK_BOX (gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12));

	gtk_box_append (vbox, font_radio);
	gtk_box_append (vbox, GTK_WIDGET (font_hbox));
	gtk_box_append (appearance_vbox, GTK_WIDGET (vbox));

	vbox = GTK_BOX (gtk_box_new (GTK_ORIENTATION_VERTICAL, 6));

	gtk_box_append (vbox, color_radio);
	gtk_box_append (vbox, prop->priv->colorcheck);
	gtk_box_append (vbox, prop->priv->colorbox);
	gtk_box_append (appearance_vbox, GTK_WIDGET (vbox));

	/* Add CSS style class, so the styling can be overridden by a GTK theme */
	gtk_widget_add_css_class (GTK_WIDGET (prop), "XpadPadProperties");

	g_signal_connect (prop->priv->fontcheck, "toggled", G_CALLBACK (change_font_check), prop);
	g_signal_connect (prop->priv->colorcheck, "toggled", G_CALLBACK (change_color_check), prop);
	g_signal_connect_swapped (prop->priv->fontbutton, "font-set", G_CALLBACK (change_font_face), prop);
	g_signal_connect_swapped (prop->priv->textbutton, "color-set", G_CALLBACK (change_text_color), prop);
	g_signal_connect_swapped (prop->priv->backbutton, "color-set", G_CALLBACK (change_back_color), prop);

	/* Setup initial state, which should never be seen, but just in case client doesn't set them itself, we'll be consistent. */
	gtk_check_button_set_active (GTK_CHECK_BUTTON (font_radio), TRUE);
	gtk_check_button_set_active (GTK_CHECK_BUTTON (color_radio), TRUE);
	gtk_widget_set_sensitive (prop->priv->colorbox, FALSE);
	gtk_widget_set_sensitive (prop->priv->fontbutton, FALSE);

	g_clear_object (&size_group_labels);

	GtkWidget *content = gtk_dialog_get_content_area (GTK_DIALOG (prop));
	gtk_box_append (GTK_BOX (content), GTK_WIDGET (appearance_vbox));

	gtk_widget_set_margin_top (content, 12);
	gtk_widget_set_margin_bottom (content, 12);
	gtk_widget_set_margin_start (content, 12);
	gtk_widget_set_margin_end (content, 12);
}

static void
xpad_pad_properties_response (GtkDialog *dialog, gint response)
{
	if (response == GTK_RESPONSE_CLOSE)
		gtk_window_destroy (GTK_WINDOW (dialog));
}

static void
change_font_check (GtkCheckButton *button, XpadPadProperties *prop)
{
	gtk_widget_set_sensitive (prop->priv->fontbutton, gtk_check_button_get_active (button));

	g_object_notify (G_OBJECT (prop), "follow-font-style");
}

static void
change_color_check (GtkCheckButton *button, XpadPadProperties *prop)
{
	gtk_widget_set_sensitive (prop->priv->colorbox, gtk_check_button_get_active (button));

	g_object_notify (G_OBJECT (prop), "follow-color-style");
}

static void
change_text_color (XpadPadProperties *prop)
{
	g_object_notify (G_OBJECT (prop), "text-color");
}

static void
change_back_color (XpadPadProperties *prop)
{
	g_object_notify (G_OBJECT (prop), "back-color");
}

static void
change_font_face (XpadPadProperties *prop)
{
	g_object_notify (G_OBJECT (prop), "fontname");
}

static void
xpad_pad_properties_set_property (GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
	XpadPadProperties *prop = XPAD_PAD_PROPERTIES (object);

	switch (prop_id)
	{
	case PROP_FOLLOW_FONT_STYLE:
		gtk_check_button_set_active (GTK_CHECK_BUTTON (prop->priv->fontcheck), !g_value_get_boolean (value));
		break;

	case PROP_FOLLOW_COLOR_STYLE:
		gtk_check_button_set_active (GTK_CHECK_BUTTON (prop->priv->colorcheck), !g_value_get_boolean (value));
		break;

	case PROP_BACK_COLOR:
		gtk_color_chooser_set_rgba (GTK_COLOR_CHOOSER (prop->priv->backbutton), g_value_get_boxed (value));
		break;

	case PROP_TEXT_COLOR:
		gtk_color_chooser_set_rgba (GTK_COLOR_CHOOSER (prop->priv->textbutton), g_value_get_boxed (value));
		break;

	case PROP_FONTNAME:
		gtk_font_chooser_set_font (GTK_FONT_CHOOSER (prop->priv->fontbutton), g_value_get_string (value));
		break;

	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
xpad_pad_properties_get_property (GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
	XpadPadProperties *prop = XPAD_PAD_PROPERTIES (object);

	switch (prop_id)
	{
	case PROP_FOLLOW_FONT_STYLE:
		g_value_set_boolean (value, !gtk_check_button_get_active (GTK_CHECK_BUTTON (prop->priv->fontcheck)));
		break;

	case PROP_FOLLOW_COLOR_STYLE:
		g_value_set_boolean (value, !gtk_check_button_get_active (GTK_CHECK_BUTTON (prop->priv->colorcheck)));
		break;

	case PROP_BACK_COLOR:
		gtk_color_chooser_get_rgba (GTK_COLOR_CHOOSER (prop->priv->backbutton), &prop->priv->backtmp);
		g_value_set_static_boxed (value, &prop->priv->backtmp);
		break;

	case PROP_TEXT_COLOR:
		gtk_color_chooser_get_rgba (GTK_COLOR_CHOOSER (prop->priv->textbutton), &prop->priv->texttmp);
		g_value_set_static_boxed (value, &prop->priv->texttmp);
		break;

	case PROP_FONTNAME:
		g_value_take_string (value, gtk_font_chooser_get_font (GTK_FONT_CHOOSER (prop->priv->fontbutton)));
		break;

	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}
