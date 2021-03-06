/*
 * empathy-account-settings.c - Source for EmpathyAccountSettings
 * Copyright (C) 2009 Collabora Ltd.
 * @author Sjoerd Simons <sjoerd.simons@collabora.co.uk>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */


#include <stdio.h>
#include <stdlib.h>

#include <telepathy-glib/account-manager.h>
#include <telepathy-glib/util.h>
#include <telepathy-glib/interfaces.h>
#include <telepathy-glib/gtypes.h>

#include "empathy-account-settings.h"
#include "empathy-connection-managers.h"
#include "empathy-utils.h"
#include "empathy-idle.h"

#define DEBUG_FLAG EMPATHY_DEBUG_ACCOUNT
#include <libempathy/empathy-debug.h>

#define GET_PRIV(obj) EMPATHY_GET_PRIV (obj, EmpathyAccountSettings)

G_DEFINE_TYPE(EmpathyAccountSettings, empathy_account_settings, G_TYPE_OBJECT)

enum {
  PROP_ACCOUNT = 1,
  PROP_CM_NAME,
  PROP_PROTOCOL,
  PROP_DISPLAY_NAME,
  PROP_DISPLAY_NAME_OVERRIDDEN,
  PROP_READY
};

/* private structure */
typedef struct _EmpathyAccountSettingsPriv EmpathyAccountSettingsPriv;

struct _EmpathyAccountSettingsPriv
{
  gboolean dispose_has_run;
  EmpathyConnectionManagers *managers;
  TpAccountManager *account_manager;

  TpConnectionManager *manager;

  TpAccount *account;
  gchar *cm_name;
  gchar *protocol;
  gchar *display_name;
  gchar *icon_name;
  gboolean display_name_overridden;
  gboolean ready;

  GHashTable *parameters;
  GArray *unset_parameters;
  GArray *required_params;

  gulong managers_ready_id;

  GSimpleAsyncResult *apply_result;
};

static void
empathy_account_settings_init (EmpathyAccountSettings *obj)
{
  EmpathyAccountSettingsPriv *priv = G_TYPE_INSTANCE_GET_PRIVATE ((obj),
    EMPATHY_TYPE_ACCOUNT_SETTINGS, EmpathyAccountSettingsPriv);

  obj->priv = priv;

  /* allocate any data required by the object here */
  priv->managers = empathy_connection_managers_dup_singleton ();
  priv->account_manager = tp_account_manager_dup ();

  priv->parameters = g_hash_table_new_full (g_str_hash, g_str_equal,
    g_free, (GDestroyNotify) tp_g_value_slice_free);

  priv->unset_parameters = g_array_new (TRUE, FALSE, sizeof (gchar *));
}

static void empathy_account_settings_dispose (GObject *object);
static void empathy_account_settings_finalize (GObject *object);
static void empathy_account_settings_account_ready_cb (GObject *source_object,
    GAsyncResult *result, gpointer user_data);
static void empathy_account_settings_managers_ready_cb (GObject *obj,
    GParamSpec *pspec, gpointer user_data);
static void empathy_account_settings_check_readyness (
    EmpathyAccountSettings *self);

static void
empathy_account_settings_set_property (GObject *object,
    guint prop_id,
    const GValue *value,
    GParamSpec *pspec)
{
  EmpathyAccountSettings *settings = EMPATHY_ACCOUNT_SETTINGS (object);
  EmpathyAccountSettingsPriv *priv = GET_PRIV (settings);

  switch (prop_id)
    {
      case PROP_ACCOUNT:
        priv->account = g_value_dup_object (value);
        break;
      case PROP_CM_NAME:
        priv->cm_name = g_value_dup_string (value);
        break;
      case PROP_PROTOCOL:
        priv->protocol = g_value_dup_string (value);
        break;
      case PROP_DISPLAY_NAME:
        priv->display_name = g_value_dup_string (value);
        break;
      case PROP_DISPLAY_NAME_OVERRIDDEN:
        priv->display_name_overridden = g_value_get_boolean (value);
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        break;
    }
}

static void
empathy_account_settings_get_property (GObject *object,
    guint prop_id,
    GValue *value,
    GParamSpec *pspec)
{
  EmpathyAccountSettings *settings = EMPATHY_ACCOUNT_SETTINGS (object);
  EmpathyAccountSettingsPriv *priv = GET_PRIV (settings);

  switch (prop_id)
    {
      case PROP_ACCOUNT:
        g_value_set_object (value, priv->account);
        break;
      case PROP_CM_NAME:
        g_value_set_string (value, priv->cm_name);
        break;
      case PROP_PROTOCOL:
        g_value_set_string (value, priv->protocol);
        break;
      case PROP_DISPLAY_NAME:
        g_value_set_string (value, priv->display_name);
        break;
      case PROP_DISPLAY_NAME_OVERRIDDEN:
        g_value_set_boolean (value, priv->display_name_overridden);
        break;
      case PROP_READY:
        g_value_set_boolean (value, priv->ready);
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        break;
    }
}

static void
empathy_account_settings_constructed (GObject *object)
{
  EmpathyAccountSettings *self = EMPATHY_ACCOUNT_SETTINGS (object);
  EmpathyAccountSettingsPriv *priv = GET_PRIV (self);

  if (priv->account != NULL)
    {
      g_free (priv->cm_name);
      g_free (priv->protocol);

      priv->cm_name =
        g_strdup (tp_account_get_connection_manager (priv->account));
      priv->protocol =
        g_strdup (tp_account_get_protocol (priv->account));
      priv->icon_name = g_strdup
        (tp_account_get_icon_name (priv->account));
    }
  else
    {
      priv->icon_name = empathy_protocol_icon_name (priv->protocol);
    }

  g_assert (priv->cm_name != NULL && priv->protocol != NULL);

  empathy_account_settings_check_readyness (self);

  if (!priv->ready)
    {
      tp_account_prepare_async (priv->account, NULL,
          empathy_account_settings_account_ready_cb, self);
      g_signal_connect (priv->managers, "notify::ready",
        G_CALLBACK (empathy_account_settings_managers_ready_cb), self);
    }

  if (G_OBJECT_CLASS (
        empathy_account_settings_parent_class)->constructed != NULL)
    G_OBJECT_CLASS (
        empathy_account_settings_parent_class)->constructed (object);
}


static void
empathy_account_settings_class_init (
    EmpathyAccountSettingsClass *empathy_account_settings_class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (empathy_account_settings_class);

  g_type_class_add_private (empathy_account_settings_class, sizeof
      (EmpathyAccountSettingsPriv));

  object_class->dispose = empathy_account_settings_dispose;
  object_class->finalize = empathy_account_settings_finalize;
  object_class->set_property = empathy_account_settings_set_property;
  object_class->get_property = empathy_account_settings_get_property;
  object_class->constructed = empathy_account_settings_constructed;

  g_object_class_install_property (object_class, PROP_ACCOUNT,
    g_param_spec_object ("account",
      "Account",
      "The TpAccount backing these settings",
      TP_TYPE_ACCOUNT,
      G_PARAM_STATIC_STRINGS | G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

  g_object_class_install_property (object_class, PROP_CM_NAME,
    g_param_spec_string ("connection-manager",
      "connection-manager",
      "The name of the connection manager this account uses",
      NULL,
      G_PARAM_STATIC_STRINGS | G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

  g_object_class_install_property (object_class, PROP_PROTOCOL,
    g_param_spec_string ("protocol",
      "Protocol",
      "The name of the protocol this account uses",
      NULL,
      G_PARAM_STATIC_STRINGS | G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

  g_object_class_install_property (object_class, PROP_DISPLAY_NAME,
    g_param_spec_string ("display-name",
      "display-name",
      "The display name account these settings belong to",
      NULL,
      G_PARAM_STATIC_STRINGS | G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

  g_object_class_install_property (object_class, PROP_DISPLAY_NAME_OVERRIDDEN,
      g_param_spec_boolean ("display-name-overridden",
        "display-name-overridden",
        "Whether the display name for this account has been manually "
        "overridden",
        FALSE,
        G_PARAM_STATIC_STRINGS | G_PARAM_READWRITE));

  g_object_class_install_property (object_class, PROP_READY,
    g_param_spec_boolean ("ready",
      "Ready",
      "Whether this account is ready to be used",
      FALSE,
      G_PARAM_STATIC_STRINGS | G_PARAM_READABLE));
}

static void
empathy_account_settings_dispose (GObject *object)
{
  EmpathyAccountSettings *self = EMPATHY_ACCOUNT_SETTINGS (object);
  EmpathyAccountSettingsPriv *priv = GET_PRIV (self);

  if (priv->dispose_has_run)
    return;

  priv->dispose_has_run = TRUE;

  if (priv->managers_ready_id != 0)
    g_signal_handler_disconnect (priv->managers, priv->managers_ready_id);
  priv->managers_ready_id = 0;

  if (priv->managers != NULL)
    g_object_unref (priv->managers);
  priv->managers = NULL;

  if (priv->manager != NULL)
    g_object_unref (priv->manager);
  priv->manager = NULL;

  if (priv->account_manager != NULL)
    g_object_unref (priv->account_manager);
  priv->account_manager = NULL;

  if (priv->account != NULL)
    g_object_unref (priv->account);
  priv->account = NULL;

  /* release any references held by the object here */
  if (G_OBJECT_CLASS (empathy_account_settings_parent_class)->dispose)
    G_OBJECT_CLASS (empathy_account_settings_parent_class)->dispose (object);
}

static void
empathy_account_settings_free_unset_parameters (
    EmpathyAccountSettings *settings)
{
  EmpathyAccountSettingsPriv *priv = GET_PRIV (settings);
  guint i;

  for (i = 0 ; i < priv->unset_parameters->len; i++)
    g_free (g_array_index (priv->unset_parameters, gchar *, i));

  g_array_set_size (priv->unset_parameters, 0);
}

static void
empathy_account_settings_finalize (GObject *object)
{
  EmpathyAccountSettings *self = EMPATHY_ACCOUNT_SETTINGS (object);
  EmpathyAccountSettingsPriv *priv = GET_PRIV (self);

  /* free any data held directly by the object here */
  g_free (priv->cm_name);
  g_free (priv->protocol);
  g_free (priv->display_name);
  g_free (priv->icon_name);

  if (priv->required_params != NULL)
    g_array_free (priv->required_params, TRUE);

  g_hash_table_destroy (priv->parameters);

  empathy_account_settings_free_unset_parameters (self);
  g_array_free (priv->unset_parameters, TRUE);

  G_OBJECT_CLASS (empathy_account_settings_parent_class)->finalize (object);
}

static void
empathy_account_settings_check_readyness (EmpathyAccountSettings *self)
{
  EmpathyAccountSettingsPriv *priv = GET_PRIV (self);
  const TpConnectionManagerProtocol *tp_protocol;

  if (priv->ready)
    return;

  if (priv->account != NULL
      && !tp_account_is_prepared (priv->account, TP_ACCOUNT_FEATURE_CORE))
      return;

  if (!empathy_connection_managers_is_ready (priv->managers))
    return;

  priv->manager = empathy_connection_managers_get_cm (
    priv->managers, priv->cm_name);

  if (priv->manager == NULL)
    return;

  if (priv->account != NULL)
    {
      g_free (priv->display_name);
      priv->display_name =
        g_strdup (tp_account_get_display_name (priv->account));

      g_free (priv->icon_name);
      priv->icon_name =
        g_strdup (tp_account_get_icon_name (priv->account));
    }

  tp_protocol = tp_connection_manager_get_protocol (priv->manager,
    priv->protocol);

  if (tp_protocol == NULL)
    {
      priv->manager = NULL;
      return;
    }

  if (priv->required_params == NULL)
    {
      TpConnectionManagerParam *cur;
      char *val;

      priv->required_params = g_array_new (TRUE, FALSE, sizeof (gchar *));

      for (cur = tp_protocol->params; cur->name != NULL; cur++)
        {
          if (tp_connection_manager_param_is_required (cur))
            {
              val = g_strdup (cur->name);
              g_array_append_val (priv->required_params, val);
            }
        }
    }

  g_object_ref (priv->manager);

  priv->ready = TRUE;
  g_object_notify (G_OBJECT (self), "ready");
}

static void
empathy_account_settings_account_ready_cb (GObject *source_object,
    GAsyncResult *result,
    gpointer user_data)
{
  EmpathyAccountSettings *settings = EMPATHY_ACCOUNT_SETTINGS (user_data);
  TpAccount *account = TP_ACCOUNT (source_object);
  GError *error = NULL;

  if (!tp_account_prepare_finish (account, result, &error))
    {
      DEBUG ("Failed to prepare account: %s", error->message);
      g_error_free (error);
      return;
    }

  empathy_account_settings_check_readyness (settings);
}

static void
empathy_account_settings_managers_ready_cb (GObject *object,
    GParamSpec *pspec,
    gpointer user_data)
{
  EmpathyAccountSettings *settings = EMPATHY_ACCOUNT_SETTINGS (user_data);

  empathy_account_settings_check_readyness (settings);
}

EmpathyAccountSettings *
empathy_account_settings_new (const gchar *connection_manager,
    const gchar *protocol,
    const char *display_name)
{
  return g_object_new (EMPATHY_TYPE_ACCOUNT_SETTINGS,
      "connection-manager", connection_manager,
      "protocol", protocol,
      "display-name", display_name,
      NULL);
}

EmpathyAccountSettings *
empathy_account_settings_new_for_account (TpAccount *account)
{
  return g_object_new (EMPATHY_TYPE_ACCOUNT_SETTINGS,
      "account", account,
      NULL);
}

TpConnectionManagerParam *
empathy_account_settings_get_tp_params (EmpathyAccountSettings *settings)
{
  EmpathyAccountSettingsPriv *priv = GET_PRIV (settings);
  const TpConnectionManagerProtocol *tp_protocol;

  g_return_val_if_fail (priv->manager != NULL, NULL);
  g_return_val_if_fail (priv->protocol != NULL, NULL);

  tp_protocol = tp_connection_manager_get_protocol (priv->manager,
     priv->protocol);
  if (tp_protocol == NULL)
    {
      DEBUG ("Can't retrieve TpConnectionManagerProtocol for protocol '%s'",
          priv->protocol);
      return NULL;
    }

  return tp_protocol->params;
}

gboolean
empathy_account_settings_is_ready (EmpathyAccountSettings *settings)
{
  EmpathyAccountSettingsPriv *priv = GET_PRIV (settings);

  return priv->ready;
}

const gchar *
empathy_account_settings_get_cm (EmpathyAccountSettings *settings)
{
  EmpathyAccountSettingsPriv *priv = GET_PRIV (settings);

  return priv->cm_name;
}

const gchar *
empathy_account_settings_get_protocol (EmpathyAccountSettings *settings)
{
  EmpathyAccountSettingsPriv *priv = GET_PRIV (settings);

  return priv->protocol;
}

gchar *
empathy_account_settings_get_icon_name (EmpathyAccountSettings *settings)
{
  EmpathyAccountSettingsPriv *priv = GET_PRIV (settings);

  return priv->icon_name;
}

const gchar *
empathy_account_settings_get_display_name (EmpathyAccountSettings *settings)
{
  EmpathyAccountSettingsPriv *priv = GET_PRIV (settings);

  return priv->display_name;
}

TpAccount *
empathy_account_settings_get_account (EmpathyAccountSettings *settings)
{
  EmpathyAccountSettingsPriv *priv = GET_PRIV (settings);

  return priv->account;
}

static gboolean
empathy_account_settings_is_unset (EmpathyAccountSettings *settings,
    const gchar *param)
{
  EmpathyAccountSettingsPriv *priv = GET_PRIV (settings);
  GArray *a;
  guint i;

  a = priv->unset_parameters;

  for (i = 0; i < a->len; i++)
    {
      if (!tp_strdiff (g_array_index (a, gchar *, i), param))
        return TRUE;
    }

  return FALSE;
}

static TpConnectionManagerParam *
empathy_account_settings_get_tp_param (EmpathyAccountSettings *settings,
    const gchar *param)
{
  TpConnectionManagerParam *tp_params =
      empathy_account_settings_get_tp_params (settings);
  TpConnectionManagerParam *p;

  for (p = tp_params; p != NULL && p->name != NULL; p++)
    {
      if (tp_strdiff (p->name, param))
        continue;

      return p;
    }

  return NULL;
}

static void
account_settings_remove_from_unset (EmpathyAccountSettings *settings,
    const gchar *param)
{
  EmpathyAccountSettingsPriv *priv = GET_PRIV (settings);
  guint idx;
  gchar *val;

  for (idx = 0; idx < priv->unset_parameters->len; idx++)
    {
      val = g_array_index (priv->unset_parameters, gchar *, idx);

      if (!tp_strdiff (val, param))
        {
          priv->unset_parameters =
            g_array_remove_index (priv->unset_parameters, idx);
          g_free (val);

          break;
        }
    }
}

const GValue *
empathy_account_settings_get_default (EmpathyAccountSettings *settings,
    const gchar *param)
{
  TpConnectionManagerParam *p;

  p = empathy_account_settings_get_tp_param (settings, param);

  if (p == NULL || !(p->flags & TP_CONN_MGR_PARAM_FLAG_HAS_DEFAULT))
    return NULL;

  return &(p->default_value);
}

const gchar *
empathy_account_settings_get_dbus_signature (EmpathyAccountSettings *settings,
    const gchar *param)
{
  TpConnectionManagerParam *p;

  p = empathy_account_settings_get_tp_param (settings, param);

  if (p == NULL)
    return NULL;

  return p->dbus_signature;
}

const GValue *
empathy_account_settings_get (EmpathyAccountSettings *settings,
    const gchar *param)
{
  EmpathyAccountSettingsPriv *priv = GET_PRIV (settings);
  const GValue *result = NULL;

  /* Lookup the update parameters we set */
  result = tp_asv_lookup (priv->parameters, param);
  if (result != NULL)
    return result;

  /* If the parameters isn't unset use the accounts setting if any */
  if (priv->account != NULL
      && !empathy_account_settings_is_unset (settings, param))
    {
      const GHashTable *parameters;

      parameters = tp_account_get_parameters (priv->account);
      result = tp_asv_lookup (parameters, param);

      if (result != NULL)
        return result;
    }

  /* fallback to the default */
  return empathy_account_settings_get_default (settings, param);
}

void
empathy_account_settings_unset (EmpathyAccountSettings *settings,
    const gchar *param)
{
  EmpathyAccountSettingsPriv *priv = GET_PRIV (settings);
  gchar *v;
  if (empathy_account_settings_is_unset (settings, param))
    return;

  v = g_strdup (param);

  g_array_append_val (priv->unset_parameters, v);
  g_hash_table_remove (priv->parameters, param);
}

void
empathy_account_settings_discard_changes (EmpathyAccountSettings *settings)
{
  EmpathyAccountSettingsPriv *priv = GET_PRIV (settings);

  g_hash_table_remove_all (priv->parameters);
  empathy_account_settings_free_unset_parameters (settings);
}

const gchar *
empathy_account_settings_get_string (EmpathyAccountSettings *settings,
    const gchar *param)
{
  const GValue *v;

  v = empathy_account_settings_get (settings, param);

  if (v == NULL || !G_VALUE_HOLDS_STRING (v))
    return NULL;

  return g_value_get_string (v);
}

gint32
empathy_account_settings_get_int32 (EmpathyAccountSettings *settings,
    const gchar *param)
{
  const GValue *v;
  gint32 ret = 0;

  v = empathy_account_settings_get (settings, param);

  if (v == NULL)
    return 0;

  switch G_VALUE_TYPE (v)
    {
      case G_TYPE_UCHAR:
        ret = g_value_get_uchar (v);
        break;
      case G_TYPE_INT:
        ret = g_value_get_int (v);
        break;
      case G_TYPE_UINT:
        ret = CLAMP (g_value_get_uint (v), (guint) G_MININT32,
            G_MAXINT32);
        break;
      case G_TYPE_INT64:
        ret = CLAMP (g_value_get_int64 (v), G_MININT32, G_MAXINT32);
        break;
      case G_TYPE_UINT64:
        ret = CLAMP (g_value_get_uint64 (v), (guint64) G_MININT32,
            G_MAXINT32);
        break;
      default:
        ret = 0;
        break;
    }

  return ret;
}

gint64
empathy_account_settings_get_int64 (EmpathyAccountSettings *settings,
    const gchar *param)
{
  const GValue *v;
  gint64 ret = 0;

  v = empathy_account_settings_get (settings, param);
  if (v == NULL)
    return 0;

  switch G_VALUE_TYPE (v)
    {
      case G_TYPE_UCHAR:
        ret = g_value_get_uchar (v);
        break;
      case G_TYPE_INT:
        ret = g_value_get_int (v);
        break;
      case G_TYPE_UINT:
        ret = g_value_get_uint (v);
        break;
      case G_TYPE_INT64:
        ret = g_value_get_int64 (v);
        break;
      case G_TYPE_UINT64:
        ret = CLAMP (g_value_get_uint64 (v), (guint64) G_MININT64, G_MAXINT64);
        break;
      default:
        ret = 0;
        break;
    }

  return ret;
}

guint32
empathy_account_settings_get_uint32 (EmpathyAccountSettings *settings,
    const gchar *param)
{
  const GValue *v;
  guint32 ret;

  v = empathy_account_settings_get (settings, param);
  if (v == NULL)
    return 0;

  switch G_VALUE_TYPE (v)
    {
      case G_TYPE_UCHAR:
        ret = g_value_get_uchar (v);
        break;
      case G_TYPE_INT:
        ret = MAX (0, g_value_get_int (v));
        break;
      case G_TYPE_UINT:
        ret = g_value_get_uint (v);
        break;
      case G_TYPE_INT64:
        ret = CLAMP (g_value_get_int64 (v), 0, G_MAXUINT32);
        break;
      case G_TYPE_UINT64:
        ret = MIN (g_value_get_uint64 (v), G_MAXUINT32);
        break;
      default:
        ret = 0;
        break;
    }

  return ret;
}

guint64
empathy_account_settings_get_uint64 (EmpathyAccountSettings *settings,
    const gchar *param)
{
  const GValue *v;
  guint64 ret = 0;

  v = empathy_account_settings_get (settings, param);

  if (v == NULL || !G_VALUE_HOLDS_INT (v))
    return 0;

  switch G_VALUE_TYPE (v)
    {
      case G_TYPE_UCHAR:
        ret = g_value_get_uchar (v);
        break;
      case G_TYPE_INT:
        ret = MAX (0, g_value_get_int (v));
        break;
      case G_TYPE_UINT:
        ret = g_value_get_uint (v);
        break;
      case G_TYPE_INT64:
        ret = MAX (0, g_value_get_int64 (v));
        break;
      case G_TYPE_UINT64:
        ret = g_value_get_uint64 (v);
        break;
      default:
        ret = 0;
        break;
    }

  return ret;
}

gboolean
empathy_account_settings_get_boolean (EmpathyAccountSettings *settings,
    const gchar *param)
{
  const GValue *v;

  v = empathy_account_settings_get (settings, param);

  if (v == NULL || !G_VALUE_HOLDS_BOOLEAN (v))
    return FALSE;

  return g_value_get_boolean (v);
}

void
empathy_account_settings_set_string (EmpathyAccountSettings *settings,
    const gchar *param,
    const gchar *value)
{
  EmpathyAccountSettingsPriv *priv = GET_PRIV (settings);

  tp_asv_set_string (priv->parameters, g_strdup (param), value);

  account_settings_remove_from_unset (settings, param);
}

void
empathy_account_settings_set_int32 (EmpathyAccountSettings *settings,
    const gchar *param,
    gint32 value)
{
  EmpathyAccountSettingsPriv *priv = GET_PRIV (settings);

  tp_asv_set_int32 (priv->parameters, g_strdup (param), value);

  account_settings_remove_from_unset (settings, param);
}

void
empathy_account_settings_set_int64 (EmpathyAccountSettings *settings,
    const gchar *param,
    gint64 value)
{
  EmpathyAccountSettingsPriv *priv = GET_PRIV (settings);

  tp_asv_set_int64 (priv->parameters, g_strdup (param), value);

  account_settings_remove_from_unset (settings, param);
}

void
empathy_account_settings_set_uint32 (EmpathyAccountSettings *settings,
    const gchar *param,
    guint32 value)
{
  EmpathyAccountSettingsPriv *priv = GET_PRIV (settings);

  tp_asv_set_uint32 (priv->parameters, g_strdup (param), value);

  account_settings_remove_from_unset (settings, param);
}

void
empathy_account_settings_set_uint64 (EmpathyAccountSettings *settings,
    const gchar *param,
    guint64 value)
{
  EmpathyAccountSettingsPriv *priv = GET_PRIV (settings);

  tp_asv_set_uint64 (priv->parameters, g_strdup (param), value);

  account_settings_remove_from_unset (settings, param);
}

void
empathy_account_settings_set_boolean (EmpathyAccountSettings *settings,
    const gchar *param,
    gboolean value)
{
  EmpathyAccountSettingsPriv *priv = GET_PRIV (settings);

  tp_asv_set_boolean (priv->parameters, g_strdup (param), value);

  account_settings_remove_from_unset (settings, param);
}

static void
account_settings_display_name_set_cb (GObject *src,
    GAsyncResult *res,
    gpointer user_data)
{
  GError *error = NULL;
  TpAccount *account = TP_ACCOUNT (src);
  GSimpleAsyncResult *set_result = user_data;

  tp_account_set_display_name_finish (account, res, &error);

  if (error != NULL)
    {
      g_simple_async_result_set_from_error (set_result, error);
      g_error_free (error);
    }

  g_simple_async_result_complete (set_result);
  g_object_unref (set_result);
}

void
empathy_account_settings_set_display_name_async (
  EmpathyAccountSettings *settings,
  const gchar *name,
  GAsyncReadyCallback callback,
  gpointer user_data)
{
  EmpathyAccountSettingsPriv *priv = GET_PRIV (settings);
  GSimpleAsyncResult *result;

  result = g_simple_async_result_new (G_OBJECT (settings),
      callback, user_data, empathy_account_settings_set_display_name_finish);

  if (priv->account == NULL)
    {
      if (priv->display_name != NULL)
        g_free (priv->display_name);

      priv->display_name = g_strdup (name);

      g_simple_async_result_complete_in_idle (result);

      return;
    }

  tp_account_set_display_name_async (priv->account, name,
      account_settings_display_name_set_cb, result);
}

gboolean
empathy_account_settings_set_display_name_finish (
  EmpathyAccountSettings *settings,
  GAsyncResult *result,
  GError **error)
{
  if (g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (result),
      error))
    return FALSE;

  g_return_val_if_fail (g_simple_async_result_is_valid (result,
    G_OBJECT (settings), empathy_account_settings_set_display_name_finish),
      FALSE);

  return TRUE;
}

static void
account_settings_icon_name_set_cb (GObject *src,
    GAsyncResult *res,
    gpointer user_data)
{
  GError *error = NULL;
  TpAccount *account = TP_ACCOUNT (src);
  GSimpleAsyncResult *set_result = user_data;

  tp_account_set_icon_name_finish (account, res, &error);

  if (error != NULL)
    {
      g_simple_async_result_set_from_error (set_result, error);
      g_error_free (error);
    }

  g_simple_async_result_complete (set_result);
  g_object_unref (set_result);
}

void
empathy_account_settings_set_icon_name_async (
  EmpathyAccountSettings *settings,
  const gchar *name,
  GAsyncReadyCallback callback,
  gpointer user_data)
{
  EmpathyAccountSettingsPriv *priv = GET_PRIV (settings);
  GSimpleAsyncResult *result;

  result = g_simple_async_result_new (G_OBJECT (settings),
      callback, user_data, empathy_account_settings_set_icon_name_finish);

  if (priv->account == NULL)
    {
      if (priv->icon_name != NULL)
        g_free (priv->icon_name);

      priv->icon_name = g_strdup (name);

      g_simple_async_result_complete_in_idle (result);

      return;
    }

  tp_account_set_icon_name_async (priv->account, name,
      account_settings_icon_name_set_cb, result);
}

gboolean
empathy_account_settings_set_icon_name_finish (
  EmpathyAccountSettings *settings,
  GAsyncResult *result,
  GError **error)
{
  if (g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (result),
      error))
    return FALSE;

  g_return_val_if_fail (g_simple_async_result_is_valid (result,
    G_OBJECT (settings), empathy_account_settings_set_icon_name_finish),
      FALSE);

  return TRUE;
}

static void
empathy_account_settings_account_updated (GObject *source,
    GAsyncResult *result,
    gpointer user_data)
{
  EmpathyAccountSettings *settings = EMPATHY_ACCOUNT_SETTINGS (user_data);
  EmpathyAccountSettingsPriv *priv = GET_PRIV (settings);
  GSimpleAsyncResult *r;
  GError *error = NULL;

  if (!tp_account_update_parameters_finish (TP_ACCOUNT (source),
          result, NULL, &error))
    {
      g_simple_async_result_set_from_error (priv->apply_result, error);
      g_error_free (error);
    }
  else
    {
      empathy_account_settings_discard_changes (settings);
    }

  r = priv->apply_result;
  priv->apply_result = NULL;

  g_simple_async_result_complete (r);
  g_object_unref (r);
}

static void
empathy_account_settings_created_cb (GObject *source,
    GAsyncResult *result,
    gpointer user_data)
{
  EmpathyAccountSettings *settings = EMPATHY_ACCOUNT_SETTINGS (user_data);
  EmpathyAccountSettingsPriv *priv = GET_PRIV (settings);
  TpAccount *account;
  GError *error = NULL;
  GSimpleAsyncResult *r;

  account = tp_account_manager_create_account_finish (
    TP_ACCOUNT_MANAGER (source), result, &error);

  if (account == NULL)
    {
      g_simple_async_result_set_from_error (priv->apply_result, error);
    }
  else
    {
      priv->account = g_object_ref (account);
      empathy_account_settings_discard_changes (settings);
    }

  r = priv->apply_result;
  priv->apply_result = NULL;

  g_simple_async_result_complete (r);
  g_object_unref (r);
}


static void
empathy_account_settings_do_create_account (EmpathyAccountSettings *settings)
{
  EmpathyAccountSettingsPriv *priv = GET_PRIV (settings);
  GHashTable *properties;
  TpConnectionPresenceType type;
  gchar *status;
  gchar *message;
  EmpathyIdle *idle;

  properties = tp_asv_new (NULL, NULL);

  idle = empathy_idle_dup_singleton ();
  type = empathy_idle_get_requested_presence (idle, &status, &message);
  g_object_unref (idle);

  if (type != TP_CONNECTION_PRESENCE_TYPE_UNSET)
    {
      /* Create the account with the requested presence the same as the current
        * global requested presence, but don't enable it */
      GValueArray *presence;
      GValue vtype = { 0, };
      GValue vstatus = { 0, };
      GValue vmessage = { 0, };

      presence = g_value_array_new (3);

      g_value_init (&vtype, G_TYPE_UINT);
      g_value_set_uint (&vtype, type);
      g_value_array_append (presence, &vtype);

      g_value_init (&vstatus, G_TYPE_STRING);
      g_value_take_string (&vstatus, status);
      g_value_array_append (presence, &vstatus);

      g_value_init (&vmessage, G_TYPE_STRING);
      g_value_take_string (&vmessage, message);
      g_value_array_append (presence, &vmessage);

      tp_asv_take_boxed (properties, TP_IFACE_ACCOUNT ".RequestedPresence",
        TP_STRUCT_TYPE_SIMPLE_PRESENCE, presence);
    }

  tp_asv_set_string (properties, TP_IFACE_ACCOUNT ".Icon",
      priv->icon_name);

  tp_account_manager_create_account_async (priv->account_manager,
    priv->cm_name, priv->protocol, priv->display_name,
    priv->parameters, properties,
    empathy_account_settings_created_cb,
    settings);

  g_hash_table_unref (properties);
}

static void
empathy_account_settings_manager_ready_cb (GObject *source_object,
    GAsyncResult *result,
    gpointer user_data)
{
  EmpathyAccountSettings *settings = EMPATHY_ACCOUNT_SETTINGS (user_data);
  EmpathyAccountSettingsPriv *priv = GET_PRIV (settings);
  TpAccountManager *account_manager = TP_ACCOUNT_MANAGER (source_object);
  GError *error = NULL;

  if (!tp_account_manager_prepare_finish (account_manager, result, &error))
    {
      DEBUG ("Failed to prepare account manager: %s", error->message);
      g_error_free (error);
      return;
    }

  g_assert (priv->apply_result != NULL && priv->account == NULL);
  empathy_account_settings_do_create_account (settings);
}

void
empathy_account_settings_apply_async (EmpathyAccountSettings *settings,
    GAsyncReadyCallback callback,
    gpointer user_data)
{
  EmpathyAccountSettingsPriv *priv = GET_PRIV (settings);

  if (priv->apply_result != NULL)
    {
      g_simple_async_report_error_in_idle (G_OBJECT (settings),
          callback, user_data,
          G_IO_ERROR, G_IO_ERROR_PENDING, "Applying already in progress");
      return;
    }

  priv->apply_result = g_simple_async_result_new (G_OBJECT (settings),
      callback, user_data, empathy_account_settings_apply_finish);

  if (priv->account == NULL)
    {
      tp_account_manager_prepare_async (priv->account_manager, NULL,
          empathy_account_settings_manager_ready_cb, settings);
    }
  else
    {
      tp_account_update_parameters_async (priv->account,
          priv->parameters, (const gchar **)priv->unset_parameters->data,
          empathy_account_settings_account_updated, settings);
    }
}

gboolean
empathy_account_settings_apply_finish (EmpathyAccountSettings *settings,
    GAsyncResult *result,
    GError **error)
{
  if (g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (result),
      error))
    return FALSE;

  g_return_val_if_fail (g_simple_async_result_is_valid (result,
    G_OBJECT (settings), empathy_account_settings_apply_finish), FALSE);

  return TRUE;
}

gboolean
empathy_account_settings_has_account (EmpathyAccountSettings *settings,
    TpAccount *account)
{
  EmpathyAccountSettingsPriv *priv;

  g_return_val_if_fail (EMPATHY_IS_ACCOUNT_SETTINGS (settings), FALSE);
  g_return_val_if_fail (TP_IS_ACCOUNT (account), FALSE);

  priv = GET_PRIV (settings);

  return (account == priv->account);
}

gboolean
empathy_account_settings_is_valid (EmpathyAccountSettings *settings)
{
  EmpathyAccountSettingsPriv *priv;
  guint idx;
  gchar *current;
  gboolean missed = FALSE;

  g_return_val_if_fail (EMPATHY_IS_ACCOUNT_SETTINGS (settings), FALSE);

  priv = GET_PRIV (settings);

  for (idx = 0; idx < priv->required_params->len; idx++)
    {
      current = g_array_index (priv->required_params, gchar *, idx);

      /* first, look if it's set in our own parameters */
      if (tp_asv_lookup (priv->parameters, current))
        continue;

      /* if we did not unset the parameter, look if it's in the account */
      if (priv->account != NULL &&
          !empathy_account_settings_is_unset (settings, current))
        {
          const GHashTable *account_params;

          account_params = tp_account_get_parameters (priv->account);
          if (tp_asv_lookup (account_params, current))
            continue;
        }

      missed = TRUE;
      break;
    }

  return !missed;
}
