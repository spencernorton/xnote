/*

Copyright (c) 2026 Spencer Norton

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

/* Cloud-backed note backup. xpad keeps everything that matters (note text in
   content-*, geometry/color/visibility in info-*, global style in
   default-style) in its config directory, so backing up is a matter of
   snapshotting that directory. The actual copying lives in the
   xpad-cloud-backup helper script — it mirrors the config dir to local
   snapshots and pushes them to cloud storage. This module only debounces
   triggers and spawns the helper, so the GTK process never touches a network
   mount itself (a hung CIFS mount must not be able to hang the UI). */

#include "../config.h"

#include <glib.h>

#include "xpad-backup.h"

#define HELPER_NAME		"xnote-cloud-backup"
/* Long enough to coalesce a burst of edits across pads (each already
   debounced ~4s by xpad-periodic), short enough that a freshly written note
   reaches the cloud within a minute. */
#define BACKUP_DEBOUNCE_SECONDS	45

static gchar *helper_path = NULL;
static guint timeout_id = 0;
static gboolean warned_missing = FALSE;

void
xpad_backup_init (void)
{
	/* XPAD_CLOUD_BACKUP_HELPER lets tests and uninstalled builds point at a
	   helper outside PATH. */
	const gchar *override = g_getenv ("XPAD_CLOUD_BACKUP_HELPER");

	if (override && *override)
		helper_path = g_strdup (override);
	else
		helper_path = g_find_program_in_path (HELPER_NAME);

	if (!helper_path && !warned_missing) {
		g_message ("%s not found in PATH; cloud note backup is disabled.", HELPER_NAME);
		warned_missing = TRUE;
	}
}

static gboolean
xpad_backup_is_initialised (void)
{
	/* If XPAD_CLOUD_BACKUP_HELPER is explicitly set (e.g. in tests), always
	   allow running it. Otherwise, check that keyfile.json exists before
	   spawning so uninitialised installations do not log errors or trigger
	   failed systemd units on every note edit. */
	const gchar *override = g_getenv ("XPAD_CLOUD_BACKUP_HELPER");
	if (override && *override)
		return TRUE;

	gchar *keyfile = g_build_filename (g_get_user_data_dir (), "xnote-backup", "keyfile.json", NULL);
	gboolean exists = g_file_test (keyfile, G_FILE_TEST_IS_REGULAR);
	g_free (keyfile);
	return exists;
}

static gboolean
xpad_backup_can_use_systemd_unit (void)
{
	const gchar *override = g_getenv ("XPAD_CLOUD_BACKUP_HELPER");
	if (override && *override)
		return FALSE;

	const gchar *runtime_dir = g_get_user_runtime_dir ();
	if (!runtime_dir || !*runtime_dir)
		return FALSE;

	gchar *systemd_sock = g_build_filename (runtime_dir, "systemd", "private", NULL);
	gboolean has_systemd = g_file_test (systemd_sock, G_FILE_TEST_EXISTS);
	g_free (systemd_sock);
	if (!has_systemd)
		return FALSE;

	gchar *user_unit = g_build_filename (g_get_user_config_dir (), "systemd", "user", "xnote-cloud-backup.service", NULL);
	gboolean has_unit = g_file_test ("/usr/lib/systemd/user/xnote-cloud-backup.service", G_FILE_TEST_IS_REGULAR) ||
	                    g_file_test ("/etc/systemd/user/xnote-cloud-backup.service", G_FILE_TEST_IS_REGULAR) ||
	                    g_file_test (user_unit, G_FILE_TEST_IS_REGULAR);
	g_free (user_unit);
	return has_unit;
}

static void
xpad_backup_run (void)
{
	if (!xpad_backup_is_initialised ())
		return;

	gchar *systemctl = NULL;
	if (xpad_backup_can_use_systemd_unit ())
		systemctl = g_find_program_in_path ("systemctl");

	if (systemctl) {
		GError *error = NULL;
		gchar *argv[] = { systemctl, "--user", "start", "--no-block", "xnote-cloud-backup.service", NULL };
		gboolean ok = g_spawn_async (NULL, argv, NULL,
		                             G_SPAWN_STDOUT_TO_DEV_NULL,
		                             NULL, NULL, NULL, &error);
		g_free (systemctl);
		if (ok)
			return;
		g_warning ("Failed to start xnote-cloud-backup.service via systemctl: %s", error->message);
		g_error_free (error);
	}

	GError *error = NULL;
	gchar *argv[] = { helper_path, "sync", NULL };

	if (!g_spawn_async (NULL, argv, NULL,
	                    G_SPAWN_STDOUT_TO_DEV_NULL,
	                    NULL, NULL, NULL, &error)) {
		g_warning ("Failed to launch %s: %s", HELPER_NAME, error->message);
		g_error_free (error);
	}
}

static gboolean
xpad_backup_timeout (gpointer data)
{
	(void) data;

	timeout_id = 0;
	xpad_backup_run ();

	return G_SOURCE_REMOVE;
}

/* Called after every on-disk save (content, info, delete). Restarts the
   debounce window so a backup runs once things settle. */
void
xpad_backup_schedule (void)
{
	if (!helper_path)
		return;

	if (timeout_id)
		g_source_remove (timeout_id);

	timeout_id = g_timeout_add_seconds (BACKUP_DEBOUNCE_SECONDS, xpad_backup_timeout, NULL);
}

/* On shutdown: if a backup is pending, fire it now. The spawned helper
   outlives this process. */
void
xpad_backup_flush (void)
{
	if (!timeout_id)
		return;

	g_source_remove (timeout_id);
	timeout_id = 0;
	xpad_backup_run ();
}
