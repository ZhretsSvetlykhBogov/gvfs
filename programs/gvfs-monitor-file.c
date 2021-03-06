/* GIO - GLib Input, Output and Streaming Library
 * 
 * Copyright (C) 2006-2007 Red Hat, Inc.
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
 * Author: Alexander Larsson <alexl@redhat.com>
 */

#include <config.h>

#include <stdio.h>
#include <unistd.h>
#include <locale.h>
#include <errno.h>

#include <glib.h>
#include <gio/gio.h>

static GMainLoop *main_loop;

static GOptionEntry entries[] = {
  { NULL }
};

static gboolean
file_monitor_callback (GFileMonitor* monitor,
		       GFile* child,
		       GFile* other_file,
		       GFileMonitorEvent eflags)
{
  g_print ("File Monitor Event:\n");
  g_print ("File = %s\n", g_file_get_parse_name (child));
  switch (eflags)
    {
    case G_FILE_MONITOR_EVENT_CHANGED:
      g_print ("Event = CHANGED\n");
      break;
    case G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT:
      g_print ("Event = CHANGES_DONE_HINT\n");
      break;
    case G_FILE_MONITOR_EVENT_DELETED:
      g_print ("Event = DELETED\n");
      break;
    case G_FILE_MONITOR_EVENT_CREATED:
      g_print ("Event = CREATED\n");
      break;
    case G_FILE_MONITOR_EVENT_UNMOUNTED:
      g_print ("Event = UNMOUNTED\n");
      break;
    case G_FILE_MONITOR_EVENT_PRE_UNMOUNT:
      g_print ("Event = PRE_UNMOUNT\n");
      break;
    case G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED:
      g_print ("Event = ATTRIB CHANGED\n");
      break;
    }
  
  return TRUE;
}

int
main (int argc, char *argv[])
{
  GFileMonitor* fmonitor;
  GError *error;
  GOptionContext *context;
  GFile *file;
  
  setlocale (LC_ALL, "");
  
  g_type_init ();
  
  error = NULL;
  context = g_option_context_new ("- monitor file <location>");
  g_option_context_add_main_entries (context, entries, GETTEXT_PACKAGE);
  g_option_context_parse (context, &argc, &argv, &error);
  g_option_context_free (context);
  
  if (argc > 1)
    {
      file = g_file_new_for_commandline_arg (argv[1]);
      fmonitor = g_file_monitor_file (file, G_FILE_MONITOR_WATCH_MOUNTS, NULL, NULL);
      g_signal_connect (fmonitor, "changed", (GCallback)file_monitor_callback, NULL);
    }
  
  main_loop = g_main_loop_new (NULL, FALSE);
  
  g_main_loop_run (main_loop);
  
  return 0;
}
