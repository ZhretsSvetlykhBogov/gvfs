/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/* gvfs - extensions for gio
 *
 * Copyright (C) 2006-2008 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: David Zeuthen <davidz@redhat.com>
 */

#include <config.h>

#include <glib.h>
#include <glib/gi18n-lib.h>
#include <gmodule.h>
#include <gio/gio.h>

#include "gproxyvolumemonitor.h"
#include "gproxyvolume.h"
#include "gproxymount.h"
#include "gproxydrive.h"

void
g_io_module_load (GIOModule *module)
{
  /* see gvfsproxyvolumemonitor.c:g_vfs_proxy_volume_monitor_daemon_init() */
  if (g_getenv ("GVFS_REMOTE_VOLUME_MONITOR_IGNORE") != NULL)
    goto out;

  bindtextdomain (GETTEXT_PACKAGE, GVFS_LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");

  if (!g_proxy_volume_monitor_setup_session_bus_connection ())
    goto out;

  g_proxy_drive_register (module);
  g_proxy_mount_register (module);
  g_proxy_volume_register (module);
  g_proxy_volume_monitor_register (module);
out:
  ;
 }

void
g_io_module_unload (GIOModule *module)
{
  if (g_getenv ("GVFS_REMOTE_VOLUME_MONITOR_IGNORE") != NULL)
    goto out;

  g_proxy_volume_monitor_teardown_session_bus_connection ();

out:
  ;
}
