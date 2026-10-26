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

#include "../config.h"

#include <gtk/gtk.h>

#include "xpad-pad-group.h"
#include "xpad-pad.h"
#include "xpad-app.h"

struct XpadPadGroupPrivate
{
	GSList *pads;
};

G_DEFINE_TYPE_WITH_PRIVATE (XpadPadGroup, xpad_pad_group, G_TYPE_OBJECT)

static void xpad_pad_group_save_unsaved_all (XpadPadGroup *group);
static guint xpad_pad_group_num_pads (XpadPadGroup *group);

enum {
	PROP_0
};

enum
{
	PAD_ADDED,
	PAD_REMOVED,
	LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

XpadPadGroup *
xpad_pad_group_new (void)
{
	return XPAD_PAD_GROUP (g_object_new (XPAD_TYPE_PAD_GROUP, NULL));
}

static void
xpad_pad_group_init (XpadPadGroup *group)
{
	group->priv = xpad_pad_group_get_instance_private (group);

	group->priv->pads = NULL;
}

static void
xpad_pad_group_class_init (XpadPadGroupClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	signals[PAD_ADDED] =
		g_signal_new ("pad_added",
		              G_OBJECT_CLASS_TYPE (object_class),
		              G_SIGNAL_RUN_FIRST,
		              G_STRUCT_OFFSET (XpadPadGroupClass, pad_added),
		              NULL, NULL,
		              g_cclosure_marshal_VOID__OBJECT,
		              G_TYPE_NONE,
		              1,
		              GTK_TYPE_WIDGET);

	signals[PAD_REMOVED] =
		g_signal_new ("pad_removed",
		              G_OBJECT_CLASS_TYPE (object_class),
		              G_SIGNAL_RUN_FIRST,
		              G_STRUCT_OFFSET (XpadPadGroupClass, pad_removed),
		              NULL, NULL,
		              g_cclosure_marshal_VOID__OBJECT,
		              G_TYPE_NONE,
		              1,
		              GTK_TYPE_WIDGET);
}

GSList *
xpad_pad_group_get_pads (XpadPadGroup *group)
{
	return g_slist_copy (group->priv->pads);
}

static gint
menu_title_compare (GtkWindow *a, GtkWindow *b)
{
        const gchar *window_title_a = gtk_window_get_title (a);
        const gchar *window_title_b = gtk_window_get_title (b);
        gchar *title_a = g_utf8_casefold (window_title_a ? window_title_a : "", -1);
        gchar *title_b = g_utf8_casefold (window_title_b ? window_title_b : "", -1);

        gint rv = g_utf8_collate (title_a, title_b);

        g_free (title_a);
        g_free (title_b);

        return rv;
}

GSList* xpad_pad_group_get_pads_sorted_by_title(XpadPadGroup *group) {
        /* Get all the pads */
        GSList *pads = xpad_pad_group_get_pads (group);

        /* Sort the pads by title. */
        return g_slist_sort (pads, (GCompareFunc) menu_title_compare);
}

/* Add a pad to this group */
void
xpad_pad_group_add (XpadPadGroup *group, GtkWidget *pad)
{
	/* GTK 3 toplevels sank their own floating reference; GTK 4 windows do not.
	   The group takes ownership here with ref_sink so the pad has a single
	   non-floating owner and a clean lifetime (released in
	   xpad_pad_group_remove). */
	g_object_ref_sink (pad);

	group->priv->pads = g_slist_append (group->priv->pads, XPAD_PAD (pad));
	g_signal_connect_swapped (pad, "destroy", G_CALLBACK (xpad_pad_group_remove), group);

	g_signal_emit (group, signals[PAD_ADDED], 0, pad);
}

/* Remove a pad from this group */
void
xpad_pad_group_remove (XpadPadGroup *group, GtkWidget *pad)
{
	g_return_if_fail (group != NULL);

	/* Idempotent: a pad can reach here twice — once from the explicit removal in
	   xpad_pad_delete(), and again from the "destroy" signal if/when it is finally
	   finalized. The list-find guard makes the second call a no-op and, crucially,
	   prevents a double-unref of an already-freed pad. */
	if (!g_slist_find (group->priv->pads, pad))
		return;

	group->priv->pads = g_slist_remove (group->priv->pads, XPAD_PAD (pad));
	/* Emit with the real pad (was previously cleared to NULL before the emit) so
	   listeners like the tray see which pad went away, then release the group's
	   ownership ref. */
	g_signal_emit (group, signals[PAD_REMOVED], 0, pad);
	g_object_unref (pad);
}

/* Delete all the current pads in the group */
void
xpad_pad_group_destroy_pads (XpadPadGroup *group)
{
	xpad_pad_group_save_unsaved_all(group);

	/* Remove (and thus disable) all the key accelerators before the pads get destroyed, preventing a call to a non-existing accelerator */
	g_slist_foreach (group->priv->pads, (GFunc) xpad_pad_remove_accelerator_group, NULL);
	/* Pads are GtkWindows; GTK 4 replaced gtk_widget_destroy() with gtk_window_destroy(). */
	g_slist_foreach (group->priv->pads, (GFunc) gtk_window_destroy, NULL);
}

static guint
xpad_pad_group_num_pads (XpadPadGroup *group)
{
	return g_slist_length (group->priv->pads);
}

gboolean xpad_pad_group_has_pads (XpadPadGroup *group)
{
	return xpad_pad_group_num_pads (group) != 0;
}

guint
xpad_pad_group_num_visible_pads (XpadPadGroup *group)
{
	guint num = 0;
	if (group)
	{
		GSList *i;
		for (i = group->priv->pads; i; i = i->next)
		{
			if (gtk_widget_get_visible(GTK_WIDGET(i->data)))
				num ++;
		}
	}
	return num;
}

static void xpad_pad_group_save_unsaved_all (XpadPadGroup *group) {
	if (group)
		g_slist_foreach (group->priv->pads, (GFunc) xpad_pad_save_unsaved, NULL);
}

void
xpad_pad_group_close_all (XpadPadGroup *group)
{
	if (group)
		g_slist_foreach (group->priv->pads, (GFunc) xpad_pad_close, NULL);
}

void
xpad_pad_group_show_all (XpadPadGroup *group)
{
	if (group)
		g_slist_foreach (group->priv->pads, (GFunc) gtk_widget_show, NULL);
}

/* If one of the pads is not visible show them all, else hide them all. */
void
xpad_pad_group_toggle_hide (XpadPadGroup *group)
{
	if (!group) {
		return;
	}

	GSList *nextPad = g_slist_nth (group->priv->pads, 0);

	while (nextPad != NULL) {
		if (!gtk_widget_get_visible (GTK_WIDGET(nextPad->data))) {
			xpad_pad_group_show_all (group);
			return;
		}

		nextPad = g_slist_next (nextPad);
	}

	xpad_pad_group_close_all (group);
}

