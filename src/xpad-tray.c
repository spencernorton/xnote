/*

Copyright (c) 2002 Jamis Buck
Copyright (c) 2003-2007 Michael Terry
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
 * GTK 4 dropped GtkStatusIcon and there is no GTK-4 AppIndicator binding, so
 * the tray icon is implemented here directly against the freedesktop
 * StatusNotifierItem (SNI) protocol plus its companion com.canonical.dbusmenu
 * menu protocol, spoken over GDBus. This is exactly what the GNOME
 * "AppIndicator and KStatusNotifierItem Support" extension consumes, so the
 * icon appears in the top bar with the rest of the status icons.
 *
 * The icon used is "xpad-symbolic" — a monochrome filled sticky-note glyph in
 * the standard symbolic gray (#bebebe) so GNOME's recolor pipeline can theme
 * it (light panel, HighContrast) while keeping the xpad identity.
 */

#include "../config.h"

#include <glib/gi18n.h>
#include <gio/gio.h>
#include <unistd.h>

#include "xpad-tray.h"
#include "xpad-app.h"
#include "xpad-pad.h"
#include "xpad-pad-group.h"
#include "xpad-preferences.h"
#include "xpad-settings.h"
#include "help.h"

#define SNI_ICON_NAME "xnote-symbolic"

/* Actions a tray menu row can trigger. */
typedef enum {
	TRAY_ACT_SEPARATOR,
	TRAY_ACT_NEW,
	TRAY_ACT_SHOW_ALL,
	TRAY_ACT_CLOSE_ALL,
	TRAY_ACT_PRESENT,
	TRAY_ACT_PREFERENCES,
	TRAY_ACT_HELP,
	TRAY_ACT_QUIT
} TrayAction;

typedef struct {
	gint32 id;
	gchar *label;
	gboolean enabled;
	TrayAction action;
	XpadPad *pad;      /* for TRAY_ACT_PRESENT only */
} TrayItem;

static XpadSettings *tray_settings = NULL;
static gulong tray_enabled_handler = 0;
static GDBusConnection *tray_bus = NULL;
static gchar *tray_well_known_name = NULL;
static guint tray_owner_id = 0;
static guint tray_watcher_watch_id = 0;
static guint tray_sni_reg_id = 0;
static guint tray_menu_reg_id = 0;
/* g_bus_unown_name() / g_bus_unwatch_name() can leave callbacks already
   queued in the main context. Each runtime incarnation carries this token so
   callbacks from a stopped incarnation cannot mutate (or dereference) the
   globals of a later one. Zero is reserved for "no incarnation". */
static guint tray_runtime_generation = 0;
static GDBusNodeInfo *tray_sni_info = NULL;
static GDBusNodeInfo *tray_menu_info = NULL;
static gboolean tray_active = FALSE;           /* our bus name is owned */
static gboolean tray_watcher_present = FALSE;  /* an SNI watcher exists on the bus */
static gboolean tray_own_resolved = FALSE;     /* name ownership settled either way */
static gboolean tray_watch_resolved = FALSE;   /* watcher presence settled either way */

static GPtrArray *tray_items = NULL;     /* of TrayItem* */
static guint32 tray_menu_revision = 1;
/* dbusmenu item IDs are positional (index + 1) and stable across rebuilds.
   An AppIndicator host keeps one actor per ID: with stable IDs a changed
   menu updates labels in place, while renumbering makes the host destroy
   every row and re-add them one idle at a time -- the slow fade seen when
   opening the menu after editing a note. Clicks still dispatch against the
   current model at the same position, and AboutToShow reports changes so
   the host re-reads the layout before it displays. */

/* ----------------------------------------------------------------------------
 * D-Bus introspection
 * ------------------------------------------------------------------------- */

static const gchar SNI_INTROSPECTION_XML[] =
	"<node>"
	"  <interface name='org.kde.StatusNotifierItem'>"
	"    <property name='Category' type='s' access='read'/>"
	"    <property name='Id' type='s' access='read'/>"
	"    <property name='Title' type='s' access='read'/>"
	"    <property name='Status' type='s' access='read'/>"
	"    <property name='IconName' type='s' access='read'/>"
	"    <property name='IconThemePath' type='s' access='read'/>"
	"    <property name='Menu' type='o' access='read'/>"
	"    <property name='ItemIsMenu' type='b' access='read'/>"
	"    <property name='ToolTip' type='(sa(iiay)ss)' access='read'/>"
	"    <method name='Activate'>"
	"      <arg name='x' type='i' direction='in'/>"
	"      <arg name='y' type='i' direction='in'/>"
	"    </method>"
	"    <method name='SecondaryActivate'>"
	"      <arg name='x' type='i' direction='in'/>"
	"      <arg name='y' type='i' direction='in'/>"
	"    </method>"
	"    <method name='ContextMenu'>"
	"      <arg name='x' type='i' direction='in'/>"
	"      <arg name='y' type='i' direction='in'/>"
	"    </method>"
	"    <method name='Scroll'>"
	"      <arg name='delta' type='i' direction='in'/>"
	"      <arg name='orientation' type='s' direction='in'/>"
	"    </method>"
	"    <signal name='NewIcon'/>"
	"    <signal name='NewTitle'/>"
	"    <signal name='NewStatus'><arg name='status' type='s'/></signal>"
	"  </interface>"
	"</node>";

static const gchar MENU_INTROSPECTION_XML[] =
	"<node>"
	"  <interface name='com.canonical.dbusmenu'>"
	"    <property name='Version' type='u' access='read'/>"
	"    <property name='Status' type='s' access='read'/>"
	"    <method name='GetLayout'>"
	"      <arg name='parentId' type='i' direction='in'/>"
	"      <arg name='recursionDepth' type='i' direction='in'/>"
	"      <arg name='propertyNames' type='as' direction='in'/>"
	"      <arg name='revision' type='u' direction='out'/>"
	"      <arg name='layout' type='(ia{sv}av)' direction='out'/>"
	"    </method>"
	"    <method name='GetGroupProperties'>"
	"      <arg name='ids' type='ai' direction='in'/>"
	"      <arg name='propertyNames' type='as' direction='in'/>"
	"      <arg name='properties' type='a(ia{sv})' direction='out'/>"
	"    </method>"
	"    <method name='GetProperty'>"
	"      <arg name='id' type='i' direction='in'/>"
	"      <arg name='name' type='s' direction='in'/>"
	"      <arg name='value' type='v' direction='out'/>"
	"    </method>"
	"    <method name='Event'>"
	"      <arg name='id' type='i' direction='in'/>"
	"      <arg name='eventId' type='s' direction='in'/>"
	"      <arg name='data' type='v' direction='in'/>"
	"      <arg name='timestamp' type='u' direction='in'/>"
	"    </method>"
	"    <method name='AboutToShow'>"
	"      <arg name='id' type='i' direction='in'/>"
	"      <arg name='needUpdate' type='b' direction='out'/>"
	"    </method>"
	"    <signal name='LayoutUpdated'>"
	"      <arg name='revision' type='u'/>"
	"      <arg name='parent' type='i'/>"
	"    </signal>"
	"  </interface>"
	"</node>";

/* ----------------------------------------------------------------------------
 * Menu model
 * ------------------------------------------------------------------------- */

static void
tray_item_free (gpointer data)
{
	TrayItem *item = data;
	if (item->pad)
		g_object_remove_weak_pointer (G_OBJECT (item->pad), (gpointer *) &item->pad);
	g_free (item->label);
	g_free (item);
}

static TrayItem *
tray_add_item (const gchar *label, gboolean enabled, TrayAction action)
{
	TrayItem *item = g_new0 (TrayItem, 1);
	item->id = (gint32) tray_items->len + 1;
	item->label = g_strdup (label);
	item->enabled = enabled;
	item->action = action;
	g_ptr_array_add (tray_items, item);
	return item;
}

/* Rebuilds the flat menu model from the current pad list. Returns TRUE if the
   visible content changed since the previous build. */
static gboolean
tray_rebuild_menu (void)
{
	XpadPadGroup *group = xpad_app_get_pad_group ();
	gboolean has_pads = xpad_pad_group_has_pads (group);

	GPtrArray *previous_items = tray_items;
	tray_items = g_ptr_array_new_with_free_func (tray_item_free);

	tray_add_item (_("_New"), TRUE, TRAY_ACT_NEW);
	tray_add_item (NULL, TRUE, TRAY_ACT_SEPARATOR);
	tray_add_item (_("_Show All"), has_pads, TRAY_ACT_SHOW_ALL);
	tray_add_item (_("_Close All"), has_pads, TRAY_ACT_CLOSE_ALL);
	tray_add_item (NULL, TRUE, TRAY_ACT_SEPARATOR);

	/* Live list of pads, sorted by title. */
	GSList *pads = xpad_pad_group_get_pads_sorted_by_title (group);
	gint n = 1;
	for (GSList *l = pads; l; l = l->next, n++) {
		XpadPad *pad = l->data;
		gchar *title = xpad_pad_get_title_for_menu (pad, n);
		TrayItem *item = tray_add_item (title, TRUE, TRAY_ACT_PRESENT);
		/* Weak: a pad destroyed while a host still shows the old menu must
		   NULL out here, not dangle into tray_dispatch. */
		item->pad = pad;
		g_object_add_weak_pointer (G_OBJECT (pad), (gpointer *) &item->pad);
		g_free (title);
	}
	g_slist_free (pads);

	if (n > 1)
		tray_add_item (NULL, TRUE, TRAY_ACT_SEPARATOR);

	tray_add_item (_("_Preferences"), TRUE, TRAY_ACT_PREFERENCES);
	tray_add_item (_("_Help"), TRUE, TRAY_ACT_HELP);
	tray_add_item (_("_Quit"), TRUE, TRAY_ACT_QUIT);

	/* Keep the previous items when the complete model is equal, so an
	 * unchanged menu stays cached host-side. Compare pad identity too: a
	 * same-title replacement at a position must still count as a change so
	 * the host refreshes that row's properties. */
	gboolean changed = previous_items->len != tray_items->len;
	for (guint i = 0; !changed && i < tray_items->len; i++) {
		TrayItem *old = g_ptr_array_index (previous_items, i);
		TrayItem *fresh = g_ptr_array_index (tray_items, i);
		changed = old->action != fresh->action || old->enabled != fresh->enabled ||
			old->pad != fresh->pad || g_strcmp0 (old->label, fresh->label) != 0;
	}
	if (!changed) {
		g_ptr_array_unref (tray_items);
		tray_items = previous_items;
	} else {
		g_ptr_array_unref (previous_items);
	}

	return changed;
}

static TrayItem *
tray_find_item (gint32 id)
{
	for (guint i = 0; i < tray_items->len; i++) {
		TrayItem *item = g_ptr_array_index (tray_items, i);
		if (item->id == id)
			return item;
	}
	return NULL;
}

static void
tray_dispatch (TrayItem *item)
{
	XpadPadGroup *group = xpad_app_get_pad_group ();

	switch (item->action) {
	case TRAY_ACT_NEW: {
		GtkWidget *pad = xpad_pad_new (group, tray_settings);
		if (pad)
			gtk_window_present (GTK_WINDOW (pad));
		break;
	}
	case TRAY_ACT_SHOW_ALL:
		xpad_pad_group_show_all (group);
		break;
	case TRAY_ACT_CLOSE_ALL:
		xpad_pad_group_close_all (group);
		break;
	case TRAY_ACT_PRESENT:
		if (item->pad)
			gtk_window_present (GTK_WINDOW (item->pad));
		break;
	case TRAY_ACT_PREFERENCES:
		xpad_preferences_open (tray_settings);
		break;
	case TRAY_ACT_HELP:
		show_help ();
		break;
	case TRAY_ACT_QUIT:
		xpad_app_quit ();
		break;
	case TRAY_ACT_SEPARATOR:
	default:
		break;
	}
}

/* ----------------------------------------------------------------------------
 * dbusmenu: build the (ia{sv}av) layout
 * ------------------------------------------------------------------------- */

static GVariant *
tray_item_properties (TrayItem *item)
{
	GVariantBuilder props;
	g_variant_builder_init (&props, G_VARIANT_TYPE ("a{sv}"));

	if (item->action == TRAY_ACT_SEPARATOR) {
		g_variant_builder_add (&props, "{sv}", "type", g_variant_new_string ("separator"));
	} else {
		g_variant_builder_add (&props, "{sv}", "label", g_variant_new_string (item->label ? item->label : ""));
		g_variant_builder_add (&props, "{sv}", "enabled", g_variant_new_boolean (item->enabled));
		g_variant_builder_add (&props, "{sv}", "visible", g_variant_new_boolean (TRUE));
	}

	return g_variant_builder_end (&props);
}

/* An empty "av" (no grandchildren) for leaf menu rows. */
static GVariant *
tray_empty_children (void)
{
	return g_variant_new_array (G_VARIANT_TYPE ("v"), NULL, 0);
}

static GVariant *
tray_build_layout (void)
{
	/* Children of the root: an "av", each variant wrapping a (ia{sv}av). */
	GVariantBuilder children;
	g_variant_builder_init (&children, G_VARIANT_TYPE ("av"));

	for (guint i = 0; i < tray_items->len; i++) {
		TrayItem *item = g_ptr_array_index (tray_items, i);
		GVariant *node = g_variant_new ("(i@a{sv}@av)",
			item->id,
			tray_item_properties (item),
			tray_empty_children ());
		g_variant_builder_add (&children, "v", node);
	}

	/* Root node: id 0, "submenu" display, with the items as children. */
	GVariantBuilder root_props;
	g_variant_builder_init (&root_props, G_VARIANT_TYPE ("a{sv}"));
	g_variant_builder_add (&root_props, "{sv}", "children-display", g_variant_new_string ("submenu"));

	return g_variant_new ("(i@a{sv}@av)", 0,
		g_variant_builder_end (&root_props),
		g_variant_builder_end (&children));
}

static void
tray_menu_method (GDBusConnection *connection, const gchar *sender,
                  const gchar *object_path, const gchar *interface_name,
                  const gchar *method_name, GVariant *parameters,
                  GDBusMethodInvocation *invocation, gpointer user_data)
{
	(void) connection; (void) sender; (void) object_path;
	(void) interface_name; (void) user_data;

	if (g_strcmp0 (method_name, "GetLayout") == 0) {
		g_dbus_method_invocation_return_value (invocation,
			g_variant_new ("(u@(ia{sv}av))", tray_menu_revision, tray_build_layout ()));
	}
	else if (g_strcmp0 (method_name, "GetGroupProperties") == 0) {
		GVariantBuilder out;
		g_variant_builder_init (&out, G_VARIANT_TYPE ("a(ia{sv})"));
		for (guint i = 0; i < tray_items->len; i++) {
			TrayItem *item = g_ptr_array_index (tray_items, i);
			g_variant_builder_add (&out, "(i@a{sv})", item->id, tray_item_properties (item));
		}
		g_dbus_method_invocation_return_value (invocation, g_variant_new ("(a(ia{sv}))", &out));
	}
	else if (g_strcmp0 (method_name, "GetProperty") == 0) {
		gint32 id;
		const gchar *name;
		g_variant_get (parameters, "(i&s)", &id, &name);
		TrayItem *item = tray_find_item (id);
		GVariant *value = NULL;
		if (item) {
			if (g_strcmp0 (name, "label") == 0)
				value = g_variant_new_string (item->label ? item->label : "");
			else if (g_strcmp0 (name, "enabled") == 0)
				value = g_variant_new_boolean (item->enabled);
			else if (g_strcmp0 (name, "visible") == 0)
				value = g_variant_new_boolean (TRUE);
		}
		if (!value)
			value = g_variant_new_string ("");
		g_dbus_method_invocation_return_value (invocation, g_variant_new ("(v)", value));
	}
	else if (g_strcmp0 (method_name, "Event") == 0) {
		gint32 id;
		const gchar *event_id;
		GVariant *data = NULL;
		guint32 timestamp;
		g_variant_get (parameters, "(i&svu)", &id, &event_id, &data, &timestamp);
		if (g_strcmp0 (event_id, "clicked") == 0) {
			TrayItem *item = tray_find_item (id);
			if (item && item->enabled)
				tray_dispatch (item);
		}
		if (data)
			g_variant_unref (data);
		g_dbus_method_invocation_return_value (invocation, NULL);
	}
	else if (g_strcmp0 (method_name, "AboutToShow") == 0) {
		gboolean changed = tray_rebuild_menu ();
		if (changed)
			tray_menu_revision++;
		g_dbus_method_invocation_return_value (invocation, g_variant_new ("(b)", changed));
	}
	else {
		g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
			G_DBUS_ERROR_UNKNOWN_METHOD, "Unknown dbusmenu method %s", method_name);
	}
}

static GVariant *
tray_menu_get_property (GDBusConnection *connection, const gchar *sender,
                        const gchar *object_path, const gchar *interface_name,
                        const gchar *property_name, GError **error, gpointer user_data)
{
	(void) connection; (void) sender; (void) object_path;
	(void) interface_name; (void) error; (void) user_data;

	if (g_strcmp0 (property_name, "Version") == 0)
		return g_variant_new_uint32 (3);
	if (g_strcmp0 (property_name, "Status") == 0)
		return g_variant_new_string ("normal");
	return NULL;
}

static const GDBusInterfaceVTable tray_menu_vtable = {
	tray_menu_method, tray_menu_get_property, NULL, { 0 }
};

/* ----------------------------------------------------------------------------
 * StatusNotifierItem interface
 * ------------------------------------------------------------------------- */

static void
tray_sni_method (GDBusConnection *connection, const gchar *sender,
                 const gchar *object_path, const gchar *interface_name,
                 const gchar *method_name, GVariant *parameters,
                 GDBusMethodInvocation *invocation, gpointer user_data)
{
	(void) connection; (void) sender; (void) object_path;
	(void) interface_name; (void) parameters; (void) user_data;

	/* Left-click (Activate) honours the Preferences "Tray left mouse click
	   behavior" setting: 0 = Do Nothing, 1 = Toggle Show All, 2 = List of
	   Pads (the host opens the dbusmenu itself via ItemIsMenu), 3 = New Pad. */
	if (g_strcmp0 (method_name, "Activate") == 0) {
		guint click_cfg = 1;
		if (tray_settings)
			g_object_get (tray_settings, "tray-click-configuration", &click_cfg, NULL);
		if (click_cfg == 1) {
			xpad_pad_group_toggle_hide (xpad_app_get_pad_group ());
		} else if (click_cfg == 3) {
			GtkWidget *pad = xpad_pad_new (xpad_app_get_pad_group (), tray_settings);
			if (pad)
				gtk_window_present (GTK_WINDOW (pad));
		}
		/* 0 and 2: nothing to do here. */
	}
	else if (g_strcmp0 (method_name, "SecondaryActivate") == 0) {
		GtkWidget *pad = xpad_pad_new (xpad_app_get_pad_group (), tray_settings);
		if (pad)
			gtk_window_present (GTK_WINDOW (pad));
	}
	/* ContextMenu / Scroll: nothing extra to do; the host shows the dbusmenu. */

	g_dbus_method_invocation_return_value (invocation, NULL);
}

static GVariant *
tray_sni_get_property (GDBusConnection *connection, const gchar *sender,
                       const gchar *object_path, const gchar *interface_name,
                       const gchar *property_name, GError **error, gpointer user_data)
{
	(void) connection; (void) sender; (void) object_path;
	(void) interface_name; (void) error; (void) user_data;

	if (g_strcmp0 (property_name, "Category") == 0)
		return g_variant_new_string ("ApplicationStatus");
	if (g_strcmp0 (property_name, "Id") == 0)
		return g_variant_new_string ("xnote");
	if (g_strcmp0 (property_name, "Title") == 0)
		return g_variant_new_string (g_get_application_name ());
	if (g_strcmp0 (property_name, "Status") == 0)
		return g_variant_new_string ("Active");
	if (g_strcmp0 (property_name, "IconName") == 0)
		return g_variant_new_string (SNI_ICON_NAME);
	if (g_strcmp0 (property_name, "IconThemePath") == 0)
		/* Lets hosts that honour it find xpad-symbolic before a system install. */
		return g_variant_new_string (DATADIR "/icons");
	if (g_strcmp0 (property_name, "Menu") == 0)
		return g_variant_new_object_path ("/MenuBar");
	if (g_strcmp0 (property_name, "ItemIsMenu") == 0) {
		/* "List of Pads" as the left-click behavior = the item IS a menu;
		   hosts then open the dbusmenu on left click. Read at registration
		   time by most hosts, so a change applies from the next start. */
		guint click_cfg = 1;
		if (tray_settings)
			g_object_get (tray_settings, "tray-click-configuration", &click_cfg, NULL);
		return g_variant_new_boolean (click_cfg == 2);
	}
	if (g_strcmp0 (property_name, "ToolTip") == 0)
		return g_variant_new ("(s@a(iiay)ss)", SNI_ICON_NAME,
			g_variant_new_array (G_VARIANT_TYPE ("(iiay)"), NULL, 0),
			g_get_application_name (), _("Sticky notes"));
	return NULL;
}

static const GDBusInterfaceVTable tray_sni_vtable = {
	tray_sni_method, tray_sni_get_property, NULL, { 0 }
};

/* ----------------------------------------------------------------------------
 * Registration
 * ------------------------------------------------------------------------- */

static guint
tray_advance_generation (void)
{
	tray_runtime_generation++;
	if (G_UNLIKELY (tray_runtime_generation == 0))
		tray_runtime_generation++;
	return tray_runtime_generation;
}

static gboolean
tray_callback_is_current (gpointer user_data)
{
	return GPOINTER_TO_UINT (user_data) == tray_runtime_generation;
}

static void
tray_register_with_watcher (void)
{
	g_dbus_connection_call (tray_bus,
		"org.kde.StatusNotifierWatcher",
		"/StatusNotifierWatcher",
		"org.kde.StatusNotifierWatcher",
		"RegisterStatusNotifierItem",
		g_variant_new ("(s)", tray_well_known_name),
		NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

/* The pad list changed while the menu may be closed: rebuild and tell hosts.
   (AboutToShow still refreshes right before the menu opens; this covers
   hosts that cache the layout and only refetch on LayoutUpdated.) */
static void
tray_emit_layout_updated (void)
{
	if (!tray_bus || !tray_menu_reg_id || !tray_items)
		return;

	if (tray_rebuild_menu ()) {
		tray_menu_revision++;
		g_dbus_connection_emit_signal (tray_bus, NULL, "/MenuBar",
			"com.canonical.dbusmenu", "LayoutUpdated",
			g_variant_new ("(ui)", tray_menu_revision, 0), NULL);
	}
}

static void
tray_on_pads_changed (XpadPadGroup *group, GtkWidget *pad, gpointer user_data)
{
	(void) group; (void) pad; (void) user_data;
	tray_emit_layout_updated ();
}

static void
tray_on_bus_acquired (GDBusConnection *connection, const gchar *name, gpointer user_data)
{
	if (!tray_callback_is_current (user_data))
		return;

	(void) name;
	tray_bus = connection;

	tray_sni_reg_id = g_dbus_connection_register_object (connection,
		"/StatusNotifierItem",
		tray_sni_info->interfaces[0],
		&tray_sni_vtable, NULL, NULL, NULL);

	tray_menu_reg_id = g_dbus_connection_register_object (connection,
		"/MenuBar",
		tray_menu_info->interfaces[0],
		&tray_menu_vtable, NULL, NULL, NULL);
}

static void
tray_on_name_acquired (GDBusConnection *connection, const gchar *name, gpointer user_data)
{
	if (!tray_callback_is_current (user_data))
		return;

	(void) connection; (void) name;
	tray_active = TRUE;
	tray_own_resolved = TRUE;
	if (tray_watcher_present)
		tray_register_with_watcher ();
}

static void
tray_on_name_lost (GDBusConnection *connection, const gchar *name, gpointer user_data)
{
	if (!tray_callback_is_current (user_data))
		return;

	(void) connection; (void) name;
	tray_active = FALSE;
	tray_own_resolved = TRUE;
}

/* A StatusNotifierWatcher appeared — either at startup or because GNOME
   Shell / the appindicator extension restarted. Registered items do not
   survive a watcher restart, so re-register every time one shows up. */
static void
tray_on_watcher_appeared (GDBusConnection *connection, const gchar *name,
                          const gchar *name_owner, gpointer user_data)
{
	if (!tray_callback_is_current (user_data))
		return;

	(void) connection; (void) name; (void) name_owner;
	tray_watcher_present = TRUE;
	tray_watch_resolved = TRUE;
	if (tray_active)
		tray_register_with_watcher ();
}

static void
tray_on_watcher_vanished (GDBusConnection *connection, const gchar *name, gpointer user_data)
{
	if (!tray_callback_is_current (user_data))
		return;

	(void) connection; (void) name;
	tray_watcher_present = FALSE;
	tray_watch_resolved = TRUE;
}

/* ----------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

static void
tray_runtime_stop (void)
{
	/* Invalidate first: unwatch/unown may dispatch already-queued callbacks,
	   and all runtime globals below are about to be cleared. */
	tray_advance_generation ();

	if (xpad_app_get_pad_group ())
		g_signal_handlers_disconnect_by_func (xpad_app_get_pad_group (),
			G_CALLBACK (tray_on_pads_changed), NULL);

	if (tray_bus) {
		if (tray_sni_reg_id)
			g_dbus_connection_unregister_object (tray_bus, tray_sni_reg_id);
		if (tray_menu_reg_id)
			g_dbus_connection_unregister_object (tray_bus, tray_menu_reg_id);
	}
	if (tray_watcher_watch_id)
		g_bus_unwatch_name (tray_watcher_watch_id);
	if (tray_owner_id)
		g_bus_unown_name (tray_owner_id);

	g_clear_pointer (&tray_sni_info, g_dbus_node_info_unref);
	g_clear_pointer (&tray_menu_info, g_dbus_node_info_unref);
	g_clear_pointer (&tray_items, g_ptr_array_unref);
	g_clear_pointer (&tray_well_known_name, g_free);

	tray_sni_reg_id = tray_menu_reg_id = tray_owner_id = tray_watcher_watch_id = 0;
	tray_active = tray_watcher_present = FALSE;
	tray_own_resolved = tray_watch_resolved = FALSE;
	tray_bus = NULL;
}

static void
tray_runtime_start (void)
{
	guint generation;
	gpointer generation_data;

	/* Both public init and notify::tray-enabled can reach this path. A live
	   instance must never register duplicate bus names, objects, or pad
	   handlers. */
	if (tray_owner_id || tray_watcher_watch_id || tray_items)
		return;

	g_return_if_fail (tray_settings != NULL);
	g_return_if_fail (xpad_app_get_pad_group () != NULL);

	tray_items = g_ptr_array_new_with_free_func (tray_item_free);
	tray_rebuild_menu ();

	GError *error = NULL;
	tray_sni_info = g_dbus_node_info_new_for_xml (SNI_INTROSPECTION_XML, &error);
	if (!tray_sni_info) {
		g_warning ("xnote tray: failed to parse SNI introspection: %s",
			error ? error->message : "unknown error");
		g_clear_error (&error);
		tray_runtime_stop ();
		return;
	}

	tray_menu_info = g_dbus_node_info_new_for_xml (MENU_INTROSPECTION_XML, &error);
	if (!tray_menu_info) {
		g_warning ("xnote tray: failed to parse menu introspection: %s",
			error ? error->message : "unknown error");
		g_clear_error (&error);
		tray_runtime_stop ();
		return;
	}

	tray_own_resolved = FALSE;
	tray_watch_resolved = FALSE;
	generation = tray_advance_generation ();
	generation_data = GUINT_TO_POINTER (generation);

	/* Unique well-known name per the SNI spec: org.kde.StatusNotifierItem-PID-ID */
	tray_well_known_name = g_strdup_printf ("org.kde.StatusNotifierItem-%d-1", (int) getpid ());

	tray_owner_id = g_bus_own_name (G_BUS_TYPE_SESSION,
		tray_well_known_name,
		G_BUS_NAME_OWNER_FLAGS_NONE,
		tray_on_bus_acquired,
		tray_on_name_acquired,
		tray_on_name_lost,
		generation_data, NULL);

	/* Track the watcher's lifetime: registration must be redone whenever a
	   watcher (re)appears, and has_indicator must be honest when none exists. */
	tray_watcher_watch_id = g_bus_watch_name (G_BUS_TYPE_SESSION,
		"org.kde.StatusNotifierWatcher",
		G_BUS_NAME_WATCHER_FLAGS_NONE,
		tray_on_watcher_appeared,
		tray_on_watcher_vanished,
		generation_data, NULL);

	/* Keep the dbusmenu current while it is closed, not only in AboutToShow. */
	g_signal_connect (xpad_app_get_pad_group (), "pad_added",
		G_CALLBACK (tray_on_pads_changed), NULL);
	g_signal_connect (xpad_app_get_pad_group (), "pad_removed",
		G_CALLBACK (tray_on_pads_changed), NULL);
}

static void
tray_enabled_changed (XpadSettings *settings, GParamSpec *pspec, gpointer user_data)
{
	gboolean tray_enabled;
	gboolean tray_was_armed;
	XpadPadGroup *group;
	(void) pspec;
	(void) user_data;

	g_object_get (settings, "tray-enabled", &tray_enabled, NULL);
	if (tray_enabled)
		tray_runtime_start ();
	else {
		/* The tray may be the only reachable surface while every note is
		   hidden. Reveal existing notes (or create one if none remain) before
		   removing an armed tray, matching the startup no-headless safety
		   rule. Do not do this for an initially-disabled tray: normal startup
		   has not loaded the saved pads yet. */
		tray_was_armed = tray_owner_id || tray_watcher_watch_id ||
			tray_items != NULL;
		group = xpad_app_get_pad_group ();
		if (tray_was_armed && group &&
		    xpad_pad_group_num_visible_pads (group) == 0) {
			if (xpad_pad_group_has_pads (group))
				xpad_pad_group_show_all (group);
			else {
				GtkWidget *pad = xpad_pad_new (group, tray_settings);
				if (pad)
					gtk_widget_show (pad);
			}
		}
		tray_runtime_stop ();
	}
}

void
xpad_tray_init (XpadSettings *settings)
{
	g_return_if_fail (XPAD_IS_SETTINGS (settings));

	/* Own the settings object for as long as the tray subsystem is installed.
	   Repeated init calls are harmless and never add duplicate notify handlers. */
	if (tray_settings != settings) {
		if (tray_settings && tray_enabled_handler)
			g_signal_handler_disconnect (tray_settings, tray_enabled_handler);
		tray_enabled_handler = 0;
		tray_runtime_stop ();
		g_clear_object (&tray_settings);
		tray_settings = g_object_ref (settings);
	}

	if (!tray_enabled_handler)
		tray_enabled_handler = g_signal_connect (tray_settings,
			"notify::tray-enabled", G_CALLBACK (tray_enabled_changed), NULL);

	tray_enabled_changed (tray_settings, NULL, NULL);
}

void
xpad_tray_dispose (XpadSettings *settings)
{
	(void) settings;

	if (tray_settings && tray_enabled_handler)
		g_signal_handler_disconnect (tray_settings, tray_enabled_handler);
	tray_enabled_handler = 0;

	tray_runtime_stop ();
	g_clear_object (&tray_settings);
}

gboolean
xpad_tray_has_indicator (void)
{
	/* Honest liveness: the icon is only reachable if a watcher exists to
	   host it. Owning our bus name with no watcher = no icon anywhere, and
	   callers (close-last-pad logic) must not assume otherwise. */
	return tray_active && tray_watcher_present;
}

gboolean
xpad_tray_ready (void)
{
	/* TRUE once the tray's presence question has an answer: either the tray
	   was never armed (disabled), or both the name-ownership and the
	   watcher-presence queries have resolved. The startup first-idle check
	   waits on this instead of racing g_bus_own_name. */
	if (!tray_owner_id)
		return TRUE;
	return tray_own_resolved && tray_watch_resolved;
}
