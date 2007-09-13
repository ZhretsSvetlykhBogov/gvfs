#include <config.h>

#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <glib/gi18n.h>
#include "mounttracker.h"
#include "gdbusutils.h"
#include "gmountspec.h"
#include "gvfsdaemonprotocol.h"
#include <gio/gvfserror.h>

typedef struct {
  char *key;
  char *value;
} KeyValue;

typedef struct {
  char *display_name;
  char *icon;

  /* Daemon object ref */
  char *dbus_id;
  char *object_path;

  /* Mount details */
  GMountSpec *mount_spec;
} VFSMount;

struct _GMountTracker
{
  GObject parent_instance;

  GList *mounts;
};

G_DEFINE_TYPE (GMountTracker, g_mount_tracker, G_TYPE_OBJECT);

static VFSMount *
find_vfs_mount (GMountTracker *tracker,
		const char *dbus_id,
		const char *obj_path)
{
  GList *l;
  for (l = tracker->mounts; l != NULL; l = l->next)
    {
      VFSMount *mount = l->data;

      if (strcmp (mount->dbus_id, dbus_id) == 0 &&
	  strcmp (mount->object_path, obj_path) == 0)
	return mount;
    }
  
  return NULL;
}

static VFSMount *
match_vfs_mount (GMountTracker *tracker,
		 GMountSpec *match)
{
  GList *l;
  for (l = tracker->mounts; l != NULL; l = l->next)
    {
      VFSMount *mount = l->data;

      if (g_mount_spec_match (mount->mount_spec, match))
	return mount;
    }
  
  return NULL;
}

static void
vfs_mount_free (VFSMount *mount)
{
  g_free (mount->display_name);
  g_free (mount->icon);
  g_free (mount->dbus_id);
  g_free (mount->object_path);
  g_mount_spec_unref (mount->mount_spec);
  
  g_free (mount);
}


static void
g_mount_tracker_finalize (GObject *object)
{
  GMountTracker *tracker;

  tracker = G_MOUNT_TRACKER (object);

  g_list_foreach (tracker->mounts, (GFunc)vfs_mount_free, NULL);
  g_list_free (tracker->mounts);

  if (G_OBJECT_CLASS (g_mount_tracker_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_mount_tracker_parent_class)->finalize) (object);
}

static void
g_mount_tracker_class_init (GMountTrackerClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = g_mount_tracker_finalize;
}

static void
vfs_mount_to_dbus (VFSMount *mount,
		   DBusMessageIter *iter)
{
  if (!dbus_message_iter_append_basic (iter,
				       DBUS_TYPE_STRING,
				       &mount->display_name))
    _g_dbus_oom ();
  
  if (!dbus_message_iter_append_basic (iter,
				       DBUS_TYPE_STRING,
				       &mount->icon))
    _g_dbus_oom ();
	      
  if (!dbus_message_iter_append_basic (iter,
				       DBUS_TYPE_STRING,
				       &mount->dbus_id))
    _g_dbus_oom ();
  
  if (!dbus_message_iter_append_basic (iter,
				       DBUS_TYPE_OBJECT_PATH,
				       &mount->object_path))
    _g_dbus_oom ();
  
  g_mount_spec_to_dbus (iter, mount->mount_spec);
}

static void
signal_mounted_unmounted (VFSMount *mount,
			  gboolean mounted)
{
  DBusMessage *message;
  DBusMessageIter iter;
  DBusConnection *conn;

  message = dbus_message_new_signal (G_VFS_DBUS_MOUNTTRACKER_PATH,
				     G_VFS_DBUS_MOUNTTRACKER_INTERFACE,
				     mounted?"mounted":"unmounted");
  if (message == NULL)
    _g_dbus_oom ();

  dbus_message_iter_init_append (message, &iter);
  vfs_mount_to_dbus (mount, &iter);

  conn = dbus_bus_get (DBUS_BUS_SESSION, NULL);
  dbus_connection_send (conn, message, NULL);
  dbus_connection_unref (conn);
  
  dbus_message_unref (message);
}


static void
register_mount (GMountTracker *tracker,
		DBusConnection *connection,
		DBusMessage *message)
{
  VFSMount *mount;
  DBusMessage *reply;
  DBusError error;
  const char *display_name, *icon, *obj_path, *id;
  DBusMessageIter iter;
  GMountSpec *mount_spec;

  id = dbus_message_get_sender (message);

  dbus_message_iter_init (message, &iter);

  dbus_error_init (&error);
  if (_g_dbus_message_iter_get_args (&iter,
				     &error,
				     DBUS_TYPE_STRING, &display_name,
				     DBUS_TYPE_STRING, &icon,
				     DBUS_TYPE_OBJECT_PATH, &obj_path,
				     0))
    {
      if (find_vfs_mount (tracker, id, obj_path) != NULL)
	reply = dbus_message_new_error (message,
					DBUS_ERROR_INVALID_ARGS,
					"Mountpoint Already registered");
      else if ((mount_spec = g_mount_spec_from_dbus (&iter)) == NULL)
	reply = dbus_message_new_error (message,
					DBUS_ERROR_INVALID_ARGS,
					"Error in mount spec");
      else
	{
	  mount = g_new0 (VFSMount, 1);
	  mount->display_name = g_strdup (display_name);
	  mount->icon = g_strdup (icon);
	  mount->dbus_id = g_strdup (id);
	  mount->object_path = g_strdup (obj_path);
	  mount->mount_spec = mount_spec;
	  
	  tracker->mounts = g_list_prepend (tracker->mounts, mount);

	  signal_mounted_unmounted (mount, TRUE);

	  reply = dbus_message_new_method_return (message);
	}
    }
  else
    {
      reply = dbus_message_new_error (message,
				      error.name, error.message);
      dbus_error_free (&error);
    }
  
  if (reply == NULL)
    _g_dbus_oom ();
  
  dbus_connection_send (connection, reply, NULL);
}

static void
lookup_mount (GMountTracker *tracker,
	      DBusConnection *connection,
	      DBusMessage *message)
{
  VFSMount *mount;
  DBusMessage *reply;
  DBusMessageIter iter;
  GMountSpec *spec;
  GError *error;

  dbus_message_iter_init (message, &iter);
  spec = g_mount_spec_from_dbus (&iter);

  if (spec != NULL)
    {
      mount = match_vfs_mount (tracker, spec);

      if (mount == NULL)
	{
	  error = NULL;
	  g_set_error (&error, G_VFS_ERROR, G_VFS_ERROR_NOT_MOUNTED,
		       _("Location is not mounted"));
	  reply = _dbus_message_new_error_from_gerror (message, error);
	  g_error_free (error);
	}
      else
	{
	  reply = dbus_message_new_method_return (message);

	  if (reply)
	    {
	      dbus_message_iter_init_append (reply, &iter);

	      vfs_mount_to_dbus (mount, &iter);
	    }
	}
    }
  else
    reply = dbus_message_new_error (message,
				    DBUS_ERROR_INVALID_ARGS,
				    "Invalid arguments");
  
  if (reply == NULL)
    _g_dbus_oom ();

  g_mount_spec_unref (spec);
  dbus_connection_send (connection, reply, NULL);
}

static void
list_mounts (GMountTracker *tracker,
	     DBusConnection *connection,
	     DBusMessage *message)
{
  VFSMount *mount;
  DBusMessage *reply;
  DBusMessageIter iter, array_iter, struct_iter;
  GList *l;

  reply = dbus_message_new_method_return (message);
  if (reply == NULL)
    _g_dbus_oom ();

  dbus_message_iter_init_append (reply, &iter);

  
  if (!dbus_message_iter_open_container (&iter,
					 DBUS_TYPE_ARRAY,
					 DBUS_STRUCT_BEGIN_CHAR_AS_STRING
					   DBUS_TYPE_STRING_AS_STRING
					   DBUS_TYPE_STRING_AS_STRING
					   DBUS_TYPE_STRING_AS_STRING
					   DBUS_TYPE_OBJECT_PATH_AS_STRING
 					   G_MOUNT_SPEC_TYPE_AS_STRING
					 DBUS_STRUCT_END_CHAR_AS_STRING,
					 &array_iter))
    _g_dbus_oom ();

  for (l = tracker->mounts; l != NULL; l = l->next)
    {
      mount = l->data;

      g_print ("mount: %p, name: %s, spec: %p\n", mount, mount->display_name, mount->mount_spec);
      
      if (!dbus_message_iter_open_container (&array_iter,
					     DBUS_TYPE_STRUCT,
					     DBUS_TYPE_STRING_AS_STRING
					     DBUS_TYPE_STRING_AS_STRING
					     DBUS_TYPE_STRING_AS_STRING
					     DBUS_TYPE_OBJECT_PATH_AS_STRING
					     G_MOUNT_SPEC_TYPE_AS_STRING,
					     &struct_iter))
	_g_dbus_oom ();

      vfs_mount_to_dbus (mount, &struct_iter);
      
      if (!dbus_message_iter_close_container (&array_iter, &struct_iter))
	_g_dbus_oom ();
    }

  if (!dbus_message_iter_close_container (&iter, &array_iter))
    _g_dbus_oom ();
  
  dbus_connection_send (connection, reply, NULL);
}

static DBusHandlerResult
dbus_message_function (DBusConnection  *connection,
		       DBusMessage     *message,
		       void            *user_data)
{
  GMountTracker *tracker = user_data;

  DBusHandlerResult res;
  
  res = DBUS_HANDLER_RESULT_HANDLED;
  if (dbus_message_is_method_call (message,
				   G_VFS_DBUS_MOUNTTRACKER_INTERFACE,
				   "registerMount"))
    register_mount (tracker, connection, message);
  else if (dbus_message_is_method_call (message,
					G_VFS_DBUS_MOUNTTRACKER_INTERFACE,
					G_VFS_DBUS_MOUNTTRACKER_OP_LOOKUP_MOUNT))
    lookup_mount (tracker, connection, message);
  else if (dbus_message_is_method_call (message,
					G_VFS_DBUS_MOUNTTRACKER_INTERFACE,
					"listMounts"))
    list_mounts (tracker, connection, message);
  else
    res = DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  
  return res;
}

struct DBusObjectPathVTable tracker_dbus_vtable = {
  NULL,
  dbus_message_function,
};

static void
client_disconnected (GMountTracker *tracker,
		     const char *dbus_id)
{
  GList *l, *next;

  next = NULL;
  for (l = tracker->mounts; l != NULL; l = next)
    {
      VFSMount *mount = l->data;
      next = l->next;

      if (strcmp (mount->dbus_id, dbus_id) == 0)
	{
	  signal_mounted_unmounted (mount, FALSE);
	  
	  vfs_mount_free (mount);
	  tracker->mounts = g_list_delete_link (tracker->mounts, l);
	}
    }
}

static DBusHandlerResult
mount_tracker_filter_func (DBusConnection *conn,
			   DBusMessage    *message,
			   gpointer        data)
{
  GMountTracker *tracker = data;
  const char *name, *from, *to;

  if (dbus_message_is_signal (message,
			      DBUS_INTERFACE_DBUS,
			      "NameOwnerChanged"))
    {
      if (dbus_message_get_args (message, NULL,
				 DBUS_TYPE_STRING, &name,
				 DBUS_TYPE_STRING, &from,
				 DBUS_TYPE_STRING, &to,
				 DBUS_TYPE_INVALID))
	{
	  if (*name == ':' &&  *to == 0)
	    client_disconnected (tracker, name);
	}
      
    }
  return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}


static void
g_mount_tracker_init (GMountTracker *tracker)
{
  DBusConnection *conn;
  DBusError error;
  
  conn = dbus_bus_get (DBUS_BUS_SESSION, NULL);

  if (!dbus_connection_register_object_path (conn, G_VFS_DBUS_MOUNTTRACKER_PATH,
					     &tracker_dbus_vtable, tracker))
    _g_dbus_oom ();

  if (!dbus_connection_add_filter (conn,
				   mount_tracker_filter_func, tracker, NULL))
    _g_dbus_oom ();

  
  dbus_error_init (&error);
  dbus_bus_add_match (conn,
		      "sender='org.freedesktop.DBus',"
		      "interface='org.freedesktop.DBus',"
		      "member='NameOwnerChanged'",
		      &error);
  if (dbus_error_is_set (&error))
    {
      g_warning ("Failed to add dbus match: %s\n", error.message);
      dbus_error_free (&error);
    }
}

GMountTracker *
g_mount_tracker_new (void)
{
  GMountTracker *tracker;
  
  tracker = g_object_new (G_TYPE_MOUNT_TRACKER, NULL);

  return tracker;
}