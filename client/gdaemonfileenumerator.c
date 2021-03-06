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

#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <glib/gi18n-lib.h>
#include <gdaemonfileenumerator.h>
#include <gio/gio.h>
#include <gvfsdaemondbus.h>
#include <gvfsdaemonprotocol.h>

#define OBJ_PATH_PREFIX "/org/gtk/vfs/client/enumerator/"

/* atomic */
static volatile gint path_counter = 1;

G_LOCK_DEFINE_STATIC(infos);

struct _GDaemonFileEnumerator
{
  GFileEnumerator parent;

  gint id;
  DBusConnection *sync_connection; /* NULL if async, i.e. we're listening on main dbus connection */

  /* protected by infos lock */
  GList *infos;
  gboolean done;

  /* For async ops, also protected by infos lock */
  int async_requested_files;
  GCancellable *async_cancel;
  gulong cancelled_tag;
  guint timeout_tag;
  GSimpleAsyncResult *async_res;
};

G_DEFINE_TYPE (GDaemonFileEnumerator, g_daemon_file_enumerator, G_TYPE_FILE_ENUMERATOR)

static GFileInfo *       g_daemon_file_enumerator_next_file   (GFileEnumerator  *enumerator,
							       GCancellable     *cancellable,
							       GError          **error);
static void              g_daemon_file_enumerator_next_files_async (GFileEnumerator     *enumerator,
								    int                  num_files,
								    int                  io_priority,
								    GCancellable        *cancellable,
								    GAsyncReadyCallback  callback,
								    gpointer             user_data);
static GList *           g_daemon_file_enumerator_next_files_finish (GFileEnumerator  *enumerator,
								     GAsyncResult     *result,
								     GError          **error);
static gboolean          g_daemon_file_enumerator_close       (GFileEnumerator  *enumerator,
							       GCancellable     *cancellable,
							       GError          **error);
static void              g_daemon_file_enumerator_close_async (GFileEnumerator      *enumerator,
							       int                   io_priority,
							       GCancellable         *cancellable,
							       GAsyncReadyCallback   callback,
							       gpointer              user_data);
static gboolean          g_daemon_file_enumerator_close_finish (GFileEnumerator      *enumerator,
							       GAsyncResult         *result,
							       GError              **error);
static DBusHandlerResult g_daemon_file_enumerator_dbus_filter (DBusConnection   *connection,
							       DBusMessage      *message,
							       void             *user_data);

static void
free_info_list (GList *infos)
{
  g_list_foreach (infos, (GFunc)g_object_unref, NULL);
  g_list_free (infos);
}

static void
g_daemon_file_enumerator_finalize (GObject *object)
{
  GDaemonFileEnumerator *daemon;
  char *path;

  daemon = G_DAEMON_FILE_ENUMERATOR (object);

  path = g_daemon_file_enumerator_get_object_path (daemon);
  _g_dbus_unregister_vfs_filter (path);
  g_free (path);

  free_info_list (daemon->infos);

  if (daemon->sync_connection)
    dbus_connection_unref (daemon->sync_connection);
  
  if (G_OBJECT_CLASS (g_daemon_file_enumerator_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_daemon_file_enumerator_parent_class)->finalize) (object);
}


static void
g_daemon_file_enumerator_class_init (GDaemonFileEnumeratorClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GFileEnumeratorClass *enumerator_class = G_FILE_ENUMERATOR_CLASS (klass);
  
  gobject_class->finalize = g_daemon_file_enumerator_finalize;

  enumerator_class->next_file = g_daemon_file_enumerator_next_file;
  enumerator_class->next_files_async = g_daemon_file_enumerator_next_files_async;
  enumerator_class->next_files_finish = g_daemon_file_enumerator_next_files_finish;
  enumerator_class->close_fn = g_daemon_file_enumerator_close;
  enumerator_class->close_async = g_daemon_file_enumerator_close_async;
  enumerator_class->close_finish = g_daemon_file_enumerator_close_finish;
}

static void
g_daemon_file_enumerator_init (GDaemonFileEnumerator *daemon)
{
  char *path;
  
  daemon->id = g_atomic_int_exchange_and_add (&path_counter, 1);

  path = g_daemon_file_enumerator_get_object_path (daemon);
  _g_dbus_register_vfs_filter (path, g_daemon_file_enumerator_dbus_filter,
			       G_OBJECT (daemon));
  g_free (path);
}

GDaemonFileEnumerator *
g_daemon_file_enumerator_new (GFile *file)
{
  GDaemonFileEnumerator *daemon;

  daemon = g_object_new (G_TYPE_DAEMON_FILE_ENUMERATOR,
                         "container", file,
                         NULL);
  
  return daemon;
}

/* Called with infos lock held */
static void
trigger_async_done (GDaemonFileEnumerator *daemon, gboolean ok)
{
  GList *rest, *l;

  if (ok)
    {
      l = daemon->infos;
      rest = g_list_nth (l, daemon->async_requested_files);
      if (rest)
	{
	  /* Split list */
	  rest->prev->next = NULL;
	  rest->prev = NULL;
	}
      daemon->infos = rest;
      
      g_simple_async_result_set_op_res_gpointer (daemon->async_res,
						 l,
						 (GDestroyNotify)free_info_list);
    }

  g_simple_async_result_complete_in_idle (daemon->async_res);
  
  if (daemon->cancelled_tag != 0)
    g_signal_handler_disconnect (daemon->async_cancel,
				 daemon->cancelled_tag);
  daemon->async_cancel = 0;
  daemon->cancelled_tag = 0;

  if (daemon->timeout_tag != 0)
    g_source_remove (daemon->timeout_tag);
  daemon->timeout_tag = 0;
  
  daemon->async_requested_files = 0;
  
  g_object_unref (daemon->async_res);
  daemon->async_res = NULL;
}

static DBusHandlerResult
g_daemon_file_enumerator_dbus_filter (DBusConnection     *connection,
				      DBusMessage        *message,
				      void               *user_data)
{
  GDaemonFileEnumerator *enumerator = user_data;
  const char *member;
  DBusMessageIter iter, array_iter;
  GList *infos;
  GFileInfo *info;
  
  member = dbus_message_get_member (message);

  if (strcmp (member, G_VFS_DBUS_ENUMERATOR_OP_DONE) == 0)
    {
      G_LOCK (infos);
      enumerator->done = TRUE;
      if (enumerator->async_requested_files > 0)
	trigger_async_done (enumerator, TRUE);
      G_UNLOCK (infos);
      return DBUS_HANDLER_RESULT_HANDLED;
    }
  else if (strcmp (member, G_VFS_DBUS_ENUMERATOR_OP_GOT_INFO) == 0)
    {
      infos = NULL;
      
      dbus_message_iter_init (message, &iter);
      if (dbus_message_iter_get_arg_type (&iter) == DBUS_TYPE_ARRAY &&
	  dbus_message_iter_get_element_type (&iter) == DBUS_TYPE_STRUCT)
	{
	  dbus_message_iter_recurse (&iter, &array_iter);

	  while (dbus_message_iter_get_arg_type (&array_iter) == DBUS_TYPE_STRUCT)
	    {
	      info = _g_dbus_get_file_info (&array_iter, NULL);
	      if (info)
		g_assert (G_IS_FILE_INFO (info));

	      if (info)
		infos = g_list_prepend (infos, info);

	      dbus_message_iter_next (&iter);
	    }
	}

      infos = g_list_reverse (infos);
      
      G_LOCK (infos);
      enumerator->infos = g_list_concat (enumerator->infos, infos);
      if (enumerator->async_requested_files > 0 &&
	  g_list_length (enumerator->infos) >= enumerator->async_requested_files)
	trigger_async_done (enumerator, TRUE);
      G_UNLOCK (infos);
      return DBUS_HANDLER_RESULT_HANDLED;
    }

  return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

char  *
g_daemon_file_enumerator_get_object_path (GDaemonFileEnumerator *enumerator)
{
  return g_strdup_printf (OBJ_PATH_PREFIX"%d", enumerator->id);
}

void
g_daemon_file_enumerator_set_sync_connection (GDaemonFileEnumerator *enumerator,
					      DBusConnection        *connection)
{
  enumerator->sync_connection = dbus_connection_ref (connection);
}

static GFileInfo *
g_daemon_file_enumerator_next_file (GFileEnumerator *enumerator,
				    GCancellable     *cancellable,
				    GError **error)
{
  GDaemonFileEnumerator *daemon = G_DAEMON_FILE_ENUMERATOR (enumerator);
  GFileInfo *info;
  gboolean done;
  int count;
  
  info = NULL;
  done = FALSE;
  count = 0;
  while (count++ < G_VFS_DBUS_TIMEOUT_MSECS / 100)
    {
      G_LOCK (infos);
      if (daemon->infos)
	{
	  done = TRUE;
	  info = daemon->infos->data;
	  if (info)
	    g_assert (G_IS_FILE_INFO (info));
	  daemon->infos = g_list_delete_link (daemon->infos, daemon->infos);
	}
      else if (daemon->done)
	done = TRUE;
      G_UNLOCK (infos);

      if (info)
	g_assert (G_IS_FILE_INFO (info));
      
      if (done)
	break;

      /* We sleep only 100 msecs here, not the full time because we might have
       * raced with the filter func being called after unlocking
       * and setting done or ->infos. So, we want to check it again reasonaby soon.
       */
      if (daemon->sync_connection != NULL)
	{
	  /* The initializing call for the enumerator was a sync one, and we
	     have a reference to its private connection. In order to ensure we
	     get the responses sent to that originating connection we pump it
	     here.
	     This should be safe as we're either on the thread that did the call
	     so its our connection, or its the private connection for another
	     thread. If that thread is idle the pumping won't affect anything, and
	     if it is doing something thats ok to, because we don't use filters
	     on the private sync connections so we won't cause any reentrancy
	     (except the file enumerator filter, but that is safe to run in
	     some other thread).
	  */
	  if (!dbus_connection_read_write_dispatch (daemon->sync_connection, 100))
	    break;
	}
      else
	{
	  /* The enumerator was initialized by an async call, so responses will
	     come to the async dbus connection. We can't pump that as that would
	     cause all sort of filters and stuff to run, possibly on the wrong
	     thread. If you want to do async next_files you must create the
	     enumerator asynchrounously.
	  */
	  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
			       "Can't do synchronous next_files() on a file enumerator created asynchronously");
	  return NULL;
	}
    }

  return info;
}

static void
async_cancelled (GCancellable *cancellable,
		 GDaemonFileEnumerator *daemon)
{
  g_simple_async_result_set_error (daemon->async_res,
				   G_IO_ERROR,
				   G_IO_ERROR_CANCELLED,
				   _("Operation was cancelled"));
  trigger_async_done (daemon, FALSE);
}

static gboolean
async_timeout (gpointer data)
{
  GDaemonFileEnumerator *daemon = G_DAEMON_FILE_ENUMERATOR (data);
  trigger_async_done (daemon, TRUE);
  return FALSE;
}

static void
g_daemon_file_enumerator_next_files_async (GFileEnumerator     *enumerator,
					   int                  num_files,
					   int                  io_priority,
					   GCancellable        *cancellable,
					   GAsyncReadyCallback  callback,
					   gpointer             user_data)
{
  GDaemonFileEnumerator *daemon = G_DAEMON_FILE_ENUMERATOR (enumerator);

  if (daemon->sync_connection != NULL)
    {
      /* If the enumerator was created synchronously then the connection used
       * for return messages will be the private connection for that thread.
       * We can't rely on it being pumped, so we don't support this.
       * We could possibly pump it ourselves in this case, but i'm not sure
       * how much sense this makes, so we don't for now.
       */
      g_simple_async_report_error_in_idle  (G_OBJECT (enumerator),
					    callback,
					    user_data,
					    G_IO_ERROR, G_IO_ERROR_FAILED,
					    "Can't do asynchronous next_files() on a file enumerator created synchronously");
      return;
    }
  
  G_LOCK (infos);
  daemon->async_cancel = cancellable;
  daemon->cancelled_tag = 0;
  daemon->timeout_tag = 0;
  daemon->async_requested_files = num_files;
  daemon->async_res = g_simple_async_result_new (G_OBJECT (enumerator), callback, user_data,
						 g_daemon_file_enumerator_next_files_async);

  /* Maybe we already have enough info to fulfill the requeust already */
  if (daemon->done ||
      g_list_length (daemon->infos) >= daemon->async_requested_files)
    trigger_async_done (daemon, TRUE);
  else
    {
      if (cancellable)
	daemon->cancelled_tag = g_signal_connect (cancellable, "cancelled", (GCallback)async_cancelled, daemon);
      daemon->timeout_tag = g_timeout_add (G_VFS_DBUS_TIMEOUT_MSECS,
					   async_timeout, daemon);
    }
  
  G_UNLOCK (infos);
}

static GList *
g_daemon_file_enumerator_next_files_finish (GFileEnumerator  *enumerator,
					    GAsyncResult     *result,
					    GError          **error)
{
  GList *l;

  l = g_simple_async_result_get_op_res_gpointer (G_SIMPLE_ASYNC_RESULT (result));
  /* We want the caller to own this, and not the result, so clear the result data */
  g_simple_async_result_set_op_res_gpointer (G_SIMPLE_ASYNC_RESULT (result),
					     NULL, NULL);
  
  return l;
}

static gboolean
g_daemon_file_enumerator_close (GFileEnumerator *enumerator,
				GCancellable     *cancellable,
				GError          **error)
{
  /*GDaemonFileEnumerator *daemon = G_DAEMON_FILE_ENUMERATOR (enumerator); */

  return TRUE;
}


/* We want an explicitly async version of close (doing nothing) to avoid
   the default thread-using version. */
static void
g_daemon_file_enumerator_close_async (GFileEnumerator      *enumerator,
				      int                   io_priority,
				      GCancellable         *cancellable,
				      GAsyncReadyCallback   callback,
				      gpointer              user_data)
{
  GSimpleAsyncResult *res;

  res = g_simple_async_result_new (G_OBJECT (enumerator), callback, user_data,
				   g_daemon_file_enumerator_close_async);
  g_simple_async_result_complete_in_idle (res);
  g_object_unref (res);
}

static gboolean
g_daemon_file_enumerator_close_finish (GFileEnumerator      *enumerator,
				       GAsyncResult         *result,
				       GError              **error)
{
  return TRUE;
}
