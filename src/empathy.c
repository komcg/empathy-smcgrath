/*
 * Copyright (C) 2007-2009 Collabora Ltd.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA  02110-1301  USA
 *
 * Authors: Xavier Claessens <xclaesse@gmail.com>
 */

#include <config.h>

#include <stdlib.h>
#include <errno.h>
#include <string.h>

#include <glib.h>
#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <unique/unique.h>

#if HAVE_LIBCHAMPLAIN
#include <clutter-gtk/clutter-gtk.h>
#endif

#include <libebook/e-book.h>
#include <libnotify/notify.h>

#include <telepathy-glib/account-manager.h>
#include <telepathy-glib/dbus.h>
#include <telepathy-glib/util.h>
#include <telepathy-glib/connection-manager.h>
#include <telepathy-glib/interfaces.h>

#include <libempathy/empathy-idle.h>
#include <libempathy/empathy-utils.h>
#include <libempathy/empathy-call-factory.h>
#include <libempathy/empathy-chatroom-manager.h>
#include <libempathy/empathy-account-settings.h>
#include <libempathy/empathy-connectivity.h>
#include <libempathy/empathy-connection-managers.h>
#include <libempathy/empathy-debugger.h>
#include <libempathy/empathy-dispatcher.h>
#include <libempathy/empathy-dispatch-operation.h>
#include <libempathy/empathy-log-manager.h>
#include <libempathy/empathy-ft-factory.h>
#include <libempathy/empathy-tp-chat.h>
#include <libempathy/empathy-tp-call.h>

#include <libempathy-gtk/empathy-conf.h>
#include <libempathy-gtk/empathy-ui-utils.h>
#include <libempathy-gtk/empathy-location-manager.h>

#include "empathy-account-assistant.h"
#include "empathy-accounts-dialog.h"
#include "empathy-main-window.h"
#include "empathy-status-icon.h"
#include "empathy-call-window.h"
#include "empathy-chat-window.h"
#include "empathy-ft-manager.h"
#include "empathy-import-mc4-accounts.h"

#include "extensions/extensions.h"

#define DEBUG_FLAG EMPATHY_DEBUG_OTHER
#include <libempathy/empathy-debug.h>

#include <gst/gst.h>

#define COMMAND_ACCOUNTS_DIALOG 1

static gboolean account_dialog_only = FALSE;
static gboolean start_hidden = FALSE;
static gboolean no_connect = FALSE;

static void
dispatch_cb (EmpathyDispatcher *dispatcher,
    EmpathyDispatchOperation *operation,
    gpointer user_data)
{
  GQuark channel_type;

  channel_type = empathy_dispatch_operation_get_channel_type_id (operation);

  if (channel_type == TP_IFACE_QUARK_CHANNEL_TYPE_TEXT)
    {
      EmpathyTpChat *tp_chat;
      EmpathyChat   *chat = NULL;
      const gchar   *id;

      tp_chat = EMPATHY_TP_CHAT
        (empathy_dispatch_operation_get_channel_wrapper (operation));

      id = empathy_tp_chat_get_id (tp_chat);
      if (!EMP_STR_EMPTY (id))
        {
          TpConnection *connection;
          TpAccount *account;

          connection = empathy_tp_chat_get_connection (tp_chat);
          account = empathy_get_account_for_connection (connection);
          chat = empathy_chat_window_find_chat (account, id);
        }

      if (chat)
        {
          empathy_chat_set_tp_chat (chat, tp_chat);
        }
      else
        {
          chat = empathy_chat_new (tp_chat);
          /* empathy_chat_new returns a floating reference as EmpathyChat is
           * a GtkWidget. This reference will be taken by a container
           * (a GtkNotebook) when we'll call empathy_chat_window_present_chat */
        }

      empathy_chat_window_present_chat (chat);

      empathy_dispatch_operation_claim (operation);
    }
  else if (channel_type == TP_IFACE_QUARK_CHANNEL_TYPE_STREAMED_MEDIA)
    {
      EmpathyCallFactory *factory;

      factory = empathy_call_factory_get ();
      empathy_call_factory_claim_channel (factory, operation);
    }
  else if (channel_type == TP_IFACE_QUARK_CHANNEL_TYPE_FILE_TRANSFER)
    {
      EmpathyFTFactory *factory;

      factory = empathy_ft_factory_dup_singleton ();

      /* if the operation is not incoming, don't claim it,
       * as it might have been triggered by another client, and
       * we are observing it.
       */
      if (empathy_dispatch_operation_is_incoming (operation))
        empathy_ft_factory_claim_channel (factory, operation);
    }
}

/* Salut account creation. The TpAccountManager first argument
 * must already be prepared when calling this function. */
static gboolean
should_create_salut_account (TpAccountManager *manager)
{
  gboolean salut_created = FALSE;
  GList *accounts, *l;

  /* Check if we already created a salut account */
  empathy_conf_get_bool (empathy_conf_get (),
      EMPATHY_PREFS_SALUT_ACCOUNT_CREATED,
      &salut_created);

  if (salut_created)
    {
      DEBUG ("Gconf says we already created a salut account once");
      return FALSE;
    }

  accounts = tp_account_manager_get_valid_accounts (manager);

  for (l = accounts; l != NULL;  l = g_list_next (l))
    {
      TpAccount *account = TP_ACCOUNT (l->data);

      if (!tp_strdiff (tp_account_get_protocol (account), "local-xmpp"))
        {
          salut_created = TRUE;
          break;
        }
    }

  g_list_free (accounts);

  if (salut_created)
    {
      DEBUG ("Existing salut account already exists, flagging so in gconf");
      empathy_conf_set_bool (empathy_conf_get (),
          EMPATHY_PREFS_SALUT_ACCOUNT_CREATED,
          TRUE);
    }

  return !salut_created;
}

static void
salut_account_created (GObject *source,
    GAsyncResult *result,
    gpointer user_data)
{
  EmpathyAccountSettings *settings = EMPATHY_ACCOUNT_SETTINGS (source);
  TpAccount *account;
  GError *error = NULL;

  if (!empathy_account_settings_apply_finish (settings, result, &error))
    {
      DEBUG ("Failed to create salut account: %s", error->message);
      g_error_free (error);
      return;
    }

  account = empathy_account_settings_get_account (settings);

  tp_account_set_enabled_async (account, TRUE, NULL, NULL);
  empathy_conf_set_bool (empathy_conf_get (),
      EMPATHY_PREFS_SALUT_ACCOUNT_CREATED,
      TRUE);
}

static void
use_conn_notify_cb (EmpathyConf *conf,
    const gchar *key,
    gpointer     user_data)
{
  EmpathyConnectivity *connectivity = user_data;
  gboolean     use_conn;

  if (empathy_conf_get_bool (conf, key, &use_conn))
    {
      empathy_connectivity_set_use_conn (connectivity, use_conn);
    }
}

static void
create_salut_account_am_ready_cb (GObject *source_object,
    GAsyncResult *result,
    gpointer user_data)
{
  TpAccountManager *account_manager = TP_ACCOUNT_MANAGER (source_object);
  EmpathyConnectionManagers *managers = user_data;
  EmpathyAccountSettings  *settings;
  TpConnectionManager *manager;
  const TpConnectionManagerProtocol *protocol;
  EBook      *book;
  EContact   *contact;
  gchar      *nickname = NULL;
  gchar      *first_name = NULL;
  gchar      *last_name = NULL;
  gchar      *email = NULL;
  gchar      *jid = NULL;
  GError     *error = NULL;

  if (!tp_account_manager_prepare_finish (account_manager, result, &error))
    {
      DEBUG ("Failed to prepare account manager: %s", error->message);
      g_error_free (error);
      goto out;
    }

  if (!should_create_salut_account (account_manager))
    goto out;

  manager = empathy_connection_managers_get_cm (managers, "salut");
  if (manager == NULL)
    {
      DEBUG ("Salut not installed, not making a salut account");
      goto out;
    }

  protocol = tp_connection_manager_get_protocol (manager, "local-xmpp");
  if (protocol == NULL)
    {
      DEBUG ("Salut doesn't support local-xmpp!!");
      goto out;
    }

  DEBUG ("Trying to add a salut account...");

  /* Get self EContact from EDS */
  if (!e_book_get_self (&contact, &book, &error))
    {
      DEBUG ("Failed to get self econtact: %s",
          error ? error->message : "No error given");
      g_clear_error (&error);
      goto out;
    }

  settings = empathy_account_settings_new ("salut", "local-xmpp",
      _("People nearby"));

  nickname = e_contact_get (contact, E_CONTACT_NICKNAME);
  first_name = e_contact_get (contact, E_CONTACT_GIVEN_NAME);
  last_name = e_contact_get (contact, E_CONTACT_FAMILY_NAME);
  email = e_contact_get (contact, E_CONTACT_EMAIL_1);
  jid = e_contact_get (contact, E_CONTACT_IM_JABBER_HOME_1);

  if (!tp_strdiff (nickname, "nickname"))
    {
      g_free (nickname);
      nickname = NULL;
    }

  DEBUG ("Salut account created:\nnickname=%s\nfirst-name=%s\n"
     "last-name=%s\nemail=%s\njid=%s\n",
     nickname, first_name, last_name, email, jid);

  empathy_account_settings_set_string (settings,
      "nickname", nickname ? nickname : "");
  empathy_account_settings_set_string (settings,
      "first-name", first_name ? first_name : "");
  empathy_account_settings_set_string (settings,
      "last-name", last_name ? last_name : "");
  empathy_account_settings_set_string (settings, "email", email ? email : "");
  empathy_account_settings_set_string (settings, "jid", jid ? jid : "");

  empathy_account_settings_apply_async (settings,
      salut_account_created, NULL);

  g_free (nickname);
  g_free (first_name);
  g_free (last_name);
  g_free (email);
  g_free (jid);
  g_object_unref (settings);
  g_object_unref (contact);
  g_object_unref (book);

 out:
  g_object_unref (managers);
}

static void
enable_logging_notify_cb (EmpathyConf *conf,
                         const gchar *key,
                         gpointer     user_data)
{
  EmpathyLogManager *log_manager = user_data;
  gboolean          enable_logging;

  if (empathy_conf_get_bool (conf, key, &enable_logging)) {
    empathy_log_manager_set_enabled (log_manager, enable_logging);
  }
}

static void
create_salut_account_if_needed (EmpathyConnectionManagers *managers)
{
  TpAccountManager *manager;

  manager = tp_account_manager_dup ();

  tp_account_manager_prepare_async (manager, NULL,
      create_salut_account_am_ready_cb, g_object_ref (managers));

  g_object_unref (manager);
}

static gboolean
has_non_salut_accounts (TpAccountManager *manager)
{
  gboolean ret = FALSE;
  GList *accounts, *l;

  accounts = tp_account_manager_get_valid_accounts (manager);

  for (l = accounts ; l != NULL; l = g_list_next (l))
    {
      if (tp_strdiff (tp_account_get_protocol (l->data), "local-xmpp"))
        {
          ret = TRUE;
          break;
        }
    }

  g_list_free (accounts);

  return ret;
}

static void
maybe_show_account_assistant (void)
{
  TpAccountManager *manager;
  manager = tp_account_manager_dup ();

  if (!has_non_salut_accounts (manager))
    empathy_account_assistant_show (GTK_WINDOW (empathy_main_window_get ()));

  g_object_unref (manager);
}

static gboolean
check_connection_managers_ready (EmpathyConnectionManagers *managers)
{
  if (empathy_connection_managers_is_ready (managers))
    {
      if (!empathy_import_mc4_accounts (managers) && !start_hidden)
        maybe_show_account_assistant ();

      create_salut_account_if_needed (managers);
      g_object_unref (managers);
      managers = NULL;
      return TRUE;
    }
  return FALSE;
}

static void
connection_managers_ready_cb (EmpathyConnectionManagers *managers,
    GParamSpec *spec,
    gpointer user_data)
{
  check_connection_managers_ready (managers);
}

static void
migrate_config_to_xdg_dir (void)
{
  gchar *xdg_dir, *old_dir, *xdg_filename, *old_filename;
  int i;
  GFile *xdg_file, *old_file;
  static const gchar* filenames[] = {
    "geometry.ini",
    "irc-networks.xml",
    "chatrooms.xml",
    "contact-groups.xml",
    "status-presets.xml",
    "accels.txt",
    NULL
  };

  xdg_dir = g_build_filename (g_get_user_config_dir (), PACKAGE_NAME, NULL);
  if (g_file_test (xdg_dir, G_FILE_TEST_EXISTS | G_FILE_TEST_IS_DIR))
    {
      /* xdg config dir already exists */
      g_free (xdg_dir);
      return;
    }

  old_dir = g_build_filename (g_get_home_dir (), ".gnome2",
      PACKAGE_NAME, NULL);
  if (!g_file_test (old_dir, G_FILE_TEST_EXISTS | G_FILE_TEST_IS_DIR))
    {
      /* old config dir didn't exist */
      g_free (xdg_dir);
      g_free (old_dir);
      return;
    }

  if (g_mkdir_with_parents (xdg_dir, (S_IRUSR | S_IWUSR | S_IXUSR)) == -1)
    {
      DEBUG ("Failed to create configuration directory; aborting migration");
      g_free (xdg_dir);
      g_free (old_dir);
      return;
    }

  for (i = 0; filenames[i]; i++)
    {
      old_filename = g_build_filename (old_dir, filenames[i], NULL);
      if (!g_file_test (old_filename, G_FILE_TEST_EXISTS))
        {
          g_free (old_filename);
          continue;
        }
      xdg_filename = g_build_filename (xdg_dir, filenames[i], NULL);
      old_file = g_file_new_for_path (old_filename);
      xdg_file = g_file_new_for_path (xdg_filename);

      if (!g_file_move (old_file, xdg_file, G_FILE_COPY_NONE,
          NULL, NULL, NULL, NULL))
        DEBUG ("Failed to migrate %s", filenames[i]);

      g_free (old_filename);
      g_free (xdg_filename);
      g_object_unref (old_file);
      g_object_unref (xdg_file);
    }

  g_free (xdg_dir);
  g_free (old_dir);
}

static void
do_show_accounts_ui (GtkWindow *window,
    TpAccountManager *manager)
{

  GtkWidget *ui;

  if (has_non_salut_accounts (manager))
    ui = empathy_accounts_dialog_show (window, NULL);
  else
    ui = empathy_account_assistant_show (window);

  if (account_dialog_only)
    g_signal_connect (ui, "destroy",
      G_CALLBACK (gtk_main_quit), NULL);
}

static void
account_manager_ready_for_accounts_cb (GObject *source_object,
    GAsyncResult *result,
    gpointer user_data)
{
  TpAccountManager *manager = TP_ACCOUNT_MANAGER (source_object);
  GError *error = NULL;

  if (!tp_account_manager_prepare_finish (manager, result, &error))
    {
      DEBUG ("Failed to prepare account manager: %s", error->message);
      g_error_free (error);
      return;
    }

  do_show_accounts_ui (user_data, manager);
}

static void
show_accounts_ui (GtkWindow *window,
    gboolean force)
{
  TpAccountManager *manager;

  if (!force)
    return;

  manager = tp_account_manager_dup ();

  tp_account_manager_prepare_async (manager, NULL,
      account_manager_ready_for_accounts_cb, window);

  g_object_unref (manager);
}

static UniqueResponse
unique_app_message_cb (UniqueApp *unique_app,
    gint command,
    UniqueMessageData *data,
    guint timestamp,
    gpointer user_data)
{
  GtkWindow *window = user_data;

  DEBUG ("Other instance launched, presenting the main window. "
      "Command=%d, timestamp %u", command, timestamp);

  if (command == COMMAND_ACCOUNTS_DIALOG)
    {
      show_accounts_ui (window, TRUE);
    }
  else
    {
      /* We're requested to show stuff again, disable the start hidden global
       * in case the accounts wizard wants to pop up.
       */
      start_hidden = FALSE;

      show_accounts_ui (window, FALSE);

      gtk_window_set_screen (GTK_WINDOW (window),
          unique_message_data_get_screen (data));
      gtk_window_set_startup_id (GTK_WINDOW (window),
          unique_message_data_get_startup_id (data));
      gtk_window_present_with_time (GTK_WINDOW (window), timestamp);
    }

  return UNIQUE_RESPONSE_OK;
}

static gboolean
show_version_cb (const char *option_name,
    const char *value,
    gpointer data,
    GError **error)
{
  g_print ("%s\n", PACKAGE_STRING);

  exit (EXIT_SUCCESS);

  return FALSE;
}

static void
new_incoming_transfer_cb (EmpathyFTFactory *factory,
    EmpathyFTHandler *handler,
    GError *error,
    gpointer user_data)
{
  if (error)
    empathy_ft_manager_display_error (handler, error);
  else
    empathy_receive_file_with_file_chooser (handler);
}

static void
new_ft_handler_cb (EmpathyFTFactory *factory,
    EmpathyFTHandler *handler,
    GError *error,
    gpointer user_data)
{
  if (error)
    empathy_ft_manager_display_error (handler, error);
  else
    empathy_ft_manager_add_handler (handler);

  g_object_unref (handler);
}

static void
new_call_handler_cb (EmpathyCallFactory *factory,
    EmpathyCallHandler *handler,
    gboolean outgoing,
    gpointer user_data)
{
  EmpathyCallWindow *window;

  window = empathy_call_window_new (handler);
  gtk_widget_show (GTK_WIDGET (window));
}

#ifdef ENABLE_DEBUG
static void
default_log_handler (const gchar *log_domain,
    GLogLevelFlags log_level,
    const gchar *message,
    gpointer user_data)
{
  g_log_default_handler (log_domain, log_level, message, NULL);

  /* G_LOG_DOMAIN = "empathy". No need to send empathy messages to the
   * debugger as they already have in empathy_debug. */
  if (log_level != G_LOG_LEVEL_DEBUG
      || tp_strdiff (log_domain, G_LOG_DOMAIN))
    {
        EmpathyDebugger *dbg;
        GTimeVal now;

        dbg = empathy_debugger_get_singleton ();
        g_get_current_time (&now);

        empathy_debugger_add_message (dbg, &now, log_domain,
                                      log_level, message);
    }
}
#endif /* ENABLE_DEBUG */

static void
account_manager_ready_cb (GObject *source_object,
    GAsyncResult *result,
    gpointer user_data)
{
  TpAccountManager *manager = TP_ACCOUNT_MANAGER (source_object);
  GError *error = NULL;
  EmpathyIdle *idle;
  EmpathyConnectivity *connectivity;
  gboolean autoconnect = TRUE;

  if (!tp_account_manager_prepare_finish (manager, result, &error))
    {
      DEBUG ("Failed to prepare account manager: %s", error->message);
      g_error_free (error);
      return;
    }

  /* Autoconnect */
  idle = empathy_idle_dup_singleton ();
  connectivity = empathy_connectivity_dup_singleton ();

  empathy_conf_get_bool (empathy_conf_get (),
      EMPATHY_PREFS_AUTOCONNECT, &autoconnect);
  if (autoconnect && !no_connect &&
      tp_connection_presence_type_cmp_availability
          (empathy_idle_get_state (idle), TP_CONNECTION_PRESENCE_TYPE_OFFLINE)
            <= 0)
      /* if current state is Offline, then put it online */
      empathy_idle_set_state (idle, TP_CONNECTION_PRESENCE_TYPE_AVAILABLE);

  if (should_create_salut_account (manager)
      || !empathy_import_mc4_has_imported ())
    {
      EmpathyConnectionManagers *managers;
      managers = empathy_connection_managers_dup_singleton ();

      if (!check_connection_managers_ready (managers))
        {
          g_signal_connect (managers, "notify::ready",
            G_CALLBACK (connection_managers_ready_cb), NULL);
        }
    }
  else if (!start_hidden)
    {
      maybe_show_account_assistant ();
    }

  g_object_unref (idle);
  g_object_unref (connectivity);
}

static EmpathyDispatcher *
setup_dispatcher (void)
{
  EmpathyDispatcher *d;
  GPtrArray *filters;
  struct {
    const gchar *channeltype;
    TpHandleType handletype;
  } types[] = {
    /* Text channels with handle types none, contact and room */
    { TP_IFACE_CHANNEL_TYPE_TEXT, TP_HANDLE_TYPE_NONE  },
    { TP_IFACE_CHANNEL_TYPE_TEXT, TP_HANDLE_TYPE_CONTACT  },
    { TP_IFACE_CHANNEL_TYPE_TEXT, TP_HANDLE_TYPE_ROOM  },
    /* file transfer to contacts */
    { TP_IFACE_CHANNEL_TYPE_FILE_TRANSFER, TP_HANDLE_TYPE_CONTACT  },
    /* stream media to contacts */
    { TP_IFACE_CHANNEL_TYPE_STREAMED_MEDIA, TP_HANDLE_TYPE_CONTACT  },
    /* stream tubes to contacts and rooms */
    { TP_IFACE_CHANNEL_TYPE_STREAM_TUBE, TP_HANDLE_TYPE_CONTACT  },
    { TP_IFACE_CHANNEL_TYPE_STREAM_TUBE, TP_HANDLE_TYPE_ROOM  },
    /* d-bus tubes to contacts and rooms */
    { TP_IFACE_CHANNEL_TYPE_DBUS_TUBE, TP_HANDLE_TYPE_CONTACT },
    { TP_IFACE_CHANNEL_TYPE_DBUS_TUBE, TP_HANDLE_TYPE_ROOM  },
    /* roomlists */
    { TP_IFACE_CHANNEL_TYPE_ROOM_LIST, TP_HANDLE_TYPE_NONE },
  };
  gchar *capabilities[] = {
    "org.freedesktop.Telepathy.Channel.Interface.MediaSignalling/ice-udp",
    "org.freedesktop.Telepathy.Channel.Interface.MediaSignalling/gtalk-p2p",
    NULL };
  GHashTable *asv;
  guint i;

  /* Setup the basic Client.Handler that matches our client filter */
  filters = g_ptr_array_new ();
  asv = tp_asv_new (
        TP_IFACE_CHANNEL ".ChannelType", G_TYPE_STRING,
           TP_IFACE_CHANNEL_TYPE_TEXT,
        TP_IFACE_CHANNEL ".TargetHandleType", G_TYPE_INT,
            TP_HANDLE_TYPE_CONTACT,
        NULL);
  g_ptr_array_add (filters, asv);

  d = empathy_dispatcher_new (PACKAGE_NAME, filters, NULL);

  g_ptr_array_foreach (filters, (GFunc) g_hash_table_destroy, NULL);
  g_ptr_array_free (filters, TRUE);

  /* Setup the an extended Client.Handler that matches everything we can do */
  filters = g_ptr_array_new ();
  for (i = 0 ; i < G_N_ELEMENTS (types); i++)
    {
      asv = tp_asv_new (
        TP_IFACE_CHANNEL ".ChannelType", G_TYPE_STRING, types[i].channeltype,
        TP_IFACE_CHANNEL ".TargetHandleType", G_TYPE_INT, types[i].handletype,
        NULL);

      g_ptr_array_add (filters, asv);
    }

  asv = tp_asv_new (
        TP_IFACE_CHANNEL ".ChannelType",
          G_TYPE_STRING, TP_IFACE_CHANNEL_TYPE_STREAMED_MEDIA,
        TP_IFACE_CHANNEL ".TargetHandleType",
          G_TYPE_INT, TP_HANDLE_TYPE_CONTACT,
        TP_IFACE_CHANNEL_TYPE_STREAMED_MEDIA ".InitialAudio",
          G_TYPE_BOOLEAN, TRUE,
        NULL);
  g_ptr_array_add (filters, asv);

  asv = tp_asv_new (
        TP_IFACE_CHANNEL ".ChannelType",
          G_TYPE_STRING, TP_IFACE_CHANNEL_TYPE_STREAMED_MEDIA,
        TP_IFACE_CHANNEL ".TargetHandleType",
          G_TYPE_INT, TP_HANDLE_TYPE_CONTACT,
        TP_IFACE_CHANNEL_TYPE_STREAMED_MEDIA ".InitialVideo",
          G_TYPE_BOOLEAN, TRUE,
        NULL);
  g_ptr_array_add (filters, asv);


  empathy_dispatcher_add_handler (d, PACKAGE_NAME"MoreThanMeetsTheEye",
    filters, capabilities);

  g_ptr_array_foreach (filters, (GFunc) g_hash_table_destroy, NULL);
  g_ptr_array_free (filters, TRUE);

  return d;
}

static void
account_status_changed_cb (TpAccount *account,
    guint old_status,
    guint new_status,
    guint reason,
    gchar *dbus_error_name,
    GHashTable *details,
    EmpathyChatroom *room)
{
  TpConnection *conn;

  conn = tp_account_get_connection (account);

  empathy_dispatcher_join_muc (conn,
      empathy_chatroom_get_room (room), NULL, NULL);
}

static void
account_manager_chatroom_ready_cb (GObject *source_object,
    GAsyncResult *result,
    gpointer user_data)
{
  TpAccountManager *account_manager = TP_ACCOUNT_MANAGER (source_object);
  EmpathyChatroomManager *chatroom_manager = user_data;
  GList *accounts, *l;
  GError *error = NULL;

  if (!tp_account_manager_prepare_finish (account_manager, result, &error))
    {
      DEBUG ("Failed to prepare account manager: %s", error->message);
      g_error_free (error);
      return;
    }

  accounts = tp_account_manager_get_valid_accounts (account_manager);

  for (l = accounts; l != NULL; l = g_list_next (l))
    {
      TpAccount *account = TP_ACCOUNT (l->data);
      TpConnection *conn;
      GList *chatrooms, *p;

      conn = tp_account_get_connection (account);

      chatrooms = empathy_chatroom_manager_get_chatrooms (
          chatroom_manager, account);

      for (p = chatrooms; p != NULL; p = p->next)
        {
          EmpathyChatroom *room = EMPATHY_CHATROOM (p->data);

          if (!empathy_chatroom_get_auto_connect (room))
            continue;

          if (conn == NULL)
            {
              g_signal_connect (G_OBJECT (account), "status-changed",
                  G_CALLBACK (account_status_changed_cb), room);
            }
          else
            {
              empathy_dispatcher_join_muc (conn,
                  empathy_chatroom_get_room (room), NULL, NULL);
            }
        }

      g_list_free (chatrooms);
    }

  g_list_free (accounts);
}

static void
chatroom_manager_ready_cb (EmpathyChatroomManager *chatroom_manager,
    GParamSpec *pspec,
    gpointer user_data)
{
  TpAccountManager *account_manager = user_data;

  tp_account_manager_prepare_async (account_manager, NULL,
      account_manager_chatroom_ready_cb, chatroom_manager);
}

int
main (int argc, char *argv[])
{
#if HAVE_GEOCLUE
  EmpathyLocationManager *location_manager = NULL;
#endif
  EmpathyStatusIcon *icon;
  EmpathyDispatcher *dispatcher;
  TpAccountManager *account_manager;
  EmpathyLogManager *log_manager;
  EmpathyChatroomManager *chatroom_manager;
  EmpathyCallFactory *call_factory;
  EmpathyFTFactory  *ft_factory;
  GtkWidget *window;
  EmpathyIdle *idle;
  EmpathyConnectivity *connectivity;
  GError *error = NULL;
  TpDBusDaemon *dbus_daemon;
  UniqueApp *unique_app;
  gboolean chatroom_manager_ready;

  GOptionContext *optcontext;
  GOptionEntry options[] = {
      { "no-connect", 'n',
        0, G_OPTION_ARG_NONE, &no_connect,
        N_("Don't connect on startup"),
        NULL },
      { "start-hidden", 'h',
        0, G_OPTION_ARG_NONE, &start_hidden,
        N_("Don't display the contact list or any other dialogs on startup"),
        NULL },
      { "accounts", 'a',
        0, G_OPTION_ARG_NONE, &account_dialog_only,
        N_("Show the accounts dialog"),
        NULL },
      { "version", 'v',
        G_OPTION_FLAG_NO_ARG, G_OPTION_ARG_CALLBACK, show_version_cb,
        NULL, NULL },
      { NULL }
  };

  /* Init */
  g_thread_init (NULL);
  empathy_init ();

  optcontext = g_option_context_new (N_("- Empathy IM Client"));
  g_option_context_add_group (optcontext, gst_init_get_option_group ());
  g_option_context_add_group (optcontext, gtk_get_option_group (TRUE));
  g_option_context_add_main_entries (optcontext, options, GETTEXT_PACKAGE);

  if (!g_option_context_parse (optcontext, &argc, &argv, &error)) {
    g_print ("%s\nRun '%s --help' to see a full list of available command line options.\n",
        error->message, argv[0]);
    g_warning ("Error in empathy init: %s", error->message);
    return EXIT_FAILURE;
  }

  g_option_context_free (optcontext);

  empathy_gtk_init ();
  g_set_application_name (_(PACKAGE_NAME));
  g_setenv ("PULSE_PROP_media.role", "phone", TRUE);

#if HAVE_LIBCHAMPLAIN
  gtk_clutter_init (&argc, &argv);
#endif

  gtk_window_set_default_icon_name ("empathy");
  textdomain (GETTEXT_PACKAGE);

#ifdef ENABLE_DEBUG
  /* Set up debugger */
  g_log_set_default_handler (default_log_handler, NULL);
#endif

  unique_app = unique_app_new_with_commands ("org.gnome.Empathy",
      NULL, "accounts_dialog", COMMAND_ACCOUNTS_DIALOG, NULL);

  if (unique_app_is_running (unique_app))
    {
      unique_app_send_message (unique_app, account_dialog_only ?
          COMMAND_ACCOUNTS_DIALOG : UNIQUE_ACTIVATE, NULL);

      g_object_unref (unique_app);
      return EXIT_SUCCESS;
    }

  /* Take well-known name */
  dbus_daemon = tp_dbus_daemon_dup (&error);
  if (error == NULL)
    {
      if (!tp_dbus_daemon_request_name (dbus_daemon,
          "org.gnome.Empathy", TRUE, &error))
        {
          DEBUG ("Failed to request well-known name: %s",
                 error ? error->message : "no message");
          g_clear_error (&error);
        }
      g_object_unref (dbus_daemon);
    }
  else
    {
      DEBUG ("Failed to dup dbus daemon: %s",
             error ? error->message : "no message");
      g_clear_error (&error);
    }

  if (account_dialog_only)
    {
      account_manager = tp_account_manager_dup ();
      show_accounts_ui (NULL, TRUE);

      gtk_main ();

      g_object_unref (account_manager);
      return 0;
    }

  notify_init (_(PACKAGE_NAME));

  /* Setting up Idle */
  idle = empathy_idle_dup_singleton ();
  empathy_idle_set_auto_away (idle, TRUE);

  /* Setting up Connectivity */
  connectivity = empathy_connectivity_dup_singleton ();
  use_conn_notify_cb (empathy_conf_get (), EMPATHY_PREFS_USE_CONN,
      connectivity);
  empathy_conf_notify_add (empathy_conf_get (), EMPATHY_PREFS_USE_CONN,
      use_conn_notify_cb, connectivity);

  /* account management */
  account_manager = tp_account_manager_dup ();
  tp_account_manager_prepare_async (account_manager, NULL,
      account_manager_ready_cb, NULL);

  /* Handle channels */
  dispatcher = setup_dispatcher ();
  g_signal_connect (dispatcher, "dispatch", G_CALLBACK (dispatch_cb), NULL);

  migrate_config_to_xdg_dir ();

  /* Setting up UI */
  window = empathy_main_window_show ();
  icon = empathy_status_icon_new (GTK_WINDOW (window), start_hidden);

  g_signal_connect (unique_app, "message-received",
      G_CALLBACK (unique_app_message_cb), window);

  /* Logging */
  log_manager = empathy_log_manager_dup_singleton ();
  empathy_log_manager_observe (log_manager, dispatcher);

  enable_logging_notify_cb (empathy_conf_get (), EMPATHY_PREFS_CHAT_ENABLE_LOGGING, log_manager);
  empathy_conf_notify_add (empathy_conf_get (), EMPATHY_PREFS_CHAT_ENABLE_LOGGING, enable_logging_notify_cb, log_manager);

  chatroom_manager = empathy_chatroom_manager_dup_singleton (NULL);
  empathy_chatroom_manager_observe (chatroom_manager, dispatcher);

  g_object_get (chatroom_manager, "ready", &chatroom_manager_ready, NULL);
  if (!chatroom_manager_ready)
    {
      g_signal_connect (G_OBJECT (chatroom_manager), "notify::ready",
          G_CALLBACK (chatroom_manager_ready_cb), account_manager);
    }
  else
    {
      chatroom_manager_ready_cb (chatroom_manager, NULL, account_manager);
    }

  /* Create the call factory */
  call_factory = empathy_call_factory_initialise ();
  g_signal_connect (G_OBJECT (call_factory), "new-call-handler",
      G_CALLBACK (new_call_handler_cb), NULL);
  /* Create the FT factory */
  ft_factory = empathy_ft_factory_dup_singleton ();
  g_signal_connect (ft_factory, "new-ft-handler",
      G_CALLBACK (new_ft_handler_cb), NULL);
  g_signal_connect (ft_factory, "new-incoming-transfer",
      G_CALLBACK (new_incoming_transfer_cb), NULL);

  /* Location mananger */
#if HAVE_GEOCLUE
  location_manager = empathy_location_manager_dup_singleton ();
#endif

  gtk_main ();

  empathy_idle_set_state (idle, TP_CONNECTION_PRESENCE_TYPE_OFFLINE);

  g_object_unref (idle);
  g_object_unref (connectivity);
  g_object_unref (icon);
  g_object_unref (account_manager);
  g_object_unref (log_manager);
  g_object_unref (dispatcher);
  g_object_unref (chatroom_manager);
#if HAVE_GEOCLUE
  g_object_unref (location_manager);
#endif
  g_object_unref (ft_factory);
  g_object_unref (unique_app);

  notify_uninit ();

  return EXIT_SUCCESS;
}
