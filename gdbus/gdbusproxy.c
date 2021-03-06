/* GDBus - GLib D-Bus Library
 *
 * Copyright (C) 2008-2009 Red Hat, Inc.
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

#include "config.h"

#include <stdlib.h>
#include <glib/gi18n.h>
#include <gobject/gvaluecollector.h>

#include "gdbusproxy.h"
#include "gdbusenumtypes.h"
#include "gdbusconnection.h"
#include "gdbuserror.h"
#include "gdbus-marshal.h"
#include "gdbusprivate.h"

/**
 * SECTION:gdbusproxy
 * @short_description: Base class for proxies
 * @include: gdbus/gdbus.h
 *
 * #GDBusProxy is a base class used for proxies to access a D-Bus
 * interface on a remote object. A #GDBusProxy can only be constructed
 * for unique name bus and does not track whether the name
 * vanishes. Use g_bus_watch_proxy() to construct #GDBusProxy proxies
 * for owners of a well-known names.
 */

struct _GDBusProxyPrivate
{
  GDBusConnection *connection;
  GDBusProxyFlags flags;
  gchar *unique_bus_name;
  gchar *object_path;
  gchar *interface_name;
  gint timeout_msec;

  /* gchar* -> GVariant* */
  GHashTable *properties;

  guint properties_changed_subscriber_id;
  guint signals_subscriber_id;
};

enum
{
  PROP_0,
  PROP_G_CONNECTION,
  PROP_G_UNIQUE_BUS_NAME,
  PROP_G_FLAGS,
  PROP_G_OBJECT_PATH,
  PROP_G_INTERFACE_NAME,
  PROP_G_DEFAULT_TIMEOUT,
};

enum
{
  PROPERTIES_CHANGED_SIGNAL,
  SIGNAL_SIGNAL,
  LAST_SIGNAL,
};

static void g_dbus_proxy_constructed (GObject *object);

guint signals[LAST_SIGNAL] = {0};

static void initable_iface_init       (GInitableIface *initable_iface);
static void async_initable_iface_init (GAsyncInitableIface *async_initable_iface);

G_DEFINE_TYPE_WITH_CODE (GDBusProxy, g_dbus_proxy, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE, initable_iface_init)
                         G_IMPLEMENT_INTERFACE (G_TYPE_ASYNC_INITABLE, async_initable_iface_init)
                         );

static void
g_dbus_proxy_finalize (GObject *object)
{
  GDBusProxy *proxy = G_DBUS_PROXY (object);

  if (proxy->priv->properties_changed_subscriber_id > 0)
    {
      g_dbus_connection_signal_unsubscribe (proxy->priv->connection,
                                            proxy->priv->properties_changed_subscriber_id);
    }

  if (proxy->priv->signals_subscriber_id > 0)
    {
      g_dbus_connection_signal_unsubscribe (proxy->priv->connection,
                                            proxy->priv->signals_subscriber_id);
    }

  g_object_unref (proxy->priv->connection);
  g_free (proxy->priv->unique_bus_name);
  g_free (proxy->priv->object_path);
  g_free (proxy->priv->interface_name);
  if (proxy->priv->properties != NULL)
    g_hash_table_unref (proxy->priv->properties);

  if (G_OBJECT_CLASS (g_dbus_proxy_parent_class)->finalize != NULL)
    G_OBJECT_CLASS (g_dbus_proxy_parent_class)->finalize (object);
}

static void
g_dbus_proxy_get_property (GObject    *object,
                           guint       prop_id,
                           GValue     *value,
                           GParamSpec *pspec)
{
  GDBusProxy *proxy = G_DBUS_PROXY (object);

  switch (prop_id)
    {
    case PROP_G_CONNECTION:
      g_value_set_object (value, proxy->priv->connection);
      break;

    case PROP_G_FLAGS:
      g_value_set_flags (value, proxy->priv->flags);
      break;

    case PROP_G_UNIQUE_BUS_NAME:
      g_value_set_string (value, proxy->priv->unique_bus_name);
      break;

    case PROP_G_OBJECT_PATH:
      g_value_set_string (value, proxy->priv->object_path);
      break;

    case PROP_G_INTERFACE_NAME:
      g_value_set_string (value, proxy->priv->interface_name);
      break;

    case PROP_G_DEFAULT_TIMEOUT:
      g_value_set_int (value, proxy->priv->timeout_msec);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
g_dbus_proxy_set_property (GObject      *object,
                           guint         prop_id,
                           const GValue *value,
                           GParamSpec   *pspec)
{
  GDBusProxy *proxy = G_DBUS_PROXY (object);

  switch (prop_id)
    {
    case PROP_G_CONNECTION:
      proxy->priv->connection = g_value_dup_object (value);
      break;

    case PROP_G_FLAGS:
      proxy->priv->flags = g_value_get_flags (value);
      break;

    case PROP_G_UNIQUE_BUS_NAME:
      proxy->priv->unique_bus_name = g_value_dup_string (value);
      break;

    case PROP_G_OBJECT_PATH:
      proxy->priv->object_path = g_value_dup_string (value);
      break;

    case PROP_G_INTERFACE_NAME:
      proxy->priv->interface_name = g_value_dup_string (value);
      break;

    case PROP_G_DEFAULT_TIMEOUT:
      g_dbus_proxy_set_default_timeout (proxy, g_value_get_int (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
g_dbus_proxy_class_init (GDBusProxyClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize     = g_dbus_proxy_finalize;
  gobject_class->set_property = g_dbus_proxy_set_property;
  gobject_class->get_property = g_dbus_proxy_get_property;
  gobject_class->constructed  = g_dbus_proxy_constructed;

  /* Note that all property names are prefixed to avoid collisions with D-Bus property names
   * in derived classes */

  /**
   * GDBusProxy:g-connection:
   *
   * The @GDBusConnection the proxy is for.
   */
  g_object_class_install_property (gobject_class,
                                   PROP_G_CONNECTION,
                                   g_param_spec_object ("g-connection",
                                                        _("g-connection"),
                                                        _("The connection the proxy is for"),
                                                        G_TYPE_DBUS_CONNECTION,
                                                        G_PARAM_READABLE |
                                                        G_PARAM_WRITABLE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_NAME |
                                                        G_PARAM_STATIC_BLURB |
                                                        G_PARAM_STATIC_NICK));

  /**
   * GDBusProxy:g-flags:
   *
   * Flags from the #GDBusProxyFlags enumeration.
   */
  g_object_class_install_property (gobject_class,
                                   PROP_G_FLAGS,
                                   g_param_spec_flags ("g-flags",
                                                       _("g-flags"),
                                                       _("Flags for the proxy"),
                                                       G_TYPE_DBUS_PROXY_FLAGS,
                                                       G_DBUS_PROXY_FLAGS_NONE,
                                                       G_PARAM_READABLE |
                                                       G_PARAM_WRITABLE |
                                                       G_PARAM_CONSTRUCT_ONLY |
                                                       G_PARAM_STATIC_NAME |
                                                       G_PARAM_STATIC_BLURB |
                                                       G_PARAM_STATIC_NICK));

  /**
   * GDBusProxy:g-unique-bus-name:
   *
   * The unique bus name the proxy is for.
   */
  g_object_class_install_property (gobject_class,
                                   PROP_G_UNIQUE_BUS_NAME,
                                   g_param_spec_string ("g-unique-bus-name",
                                                        _("g-unique-bus-name"),
                                                        _("The unique bus name the proxy is for"),
                                                        NULL,
                                                        G_PARAM_READABLE |
                                                        G_PARAM_WRITABLE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_NAME |
                                                        G_PARAM_STATIC_BLURB |
                                                        G_PARAM_STATIC_NICK));

  /**
   * GDBusProxy:g-object-path:
   *
   * The object path the proxy is for.
   */
  g_object_class_install_property (gobject_class,
                                   PROP_G_OBJECT_PATH,
                                   g_param_spec_string ("g-object-path",
                                                        _("g-object-path"),
                                                        _("The object path the proxy is for"),
                                                        NULL,
                                                        G_PARAM_READABLE |
                                                        G_PARAM_WRITABLE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_NAME |
                                                        G_PARAM_STATIC_BLURB |
                                                        G_PARAM_STATIC_NICK));

  /**
   * GDBusProxy:g-interface-name:
   *
   * The D-Bus interface name the proxy is for.
   */
  g_object_class_install_property (gobject_class,
                                   PROP_G_INTERFACE_NAME,
                                   g_param_spec_string ("g-interface-name",
                                                        _("g-interface-name"),
                                                        _("The D-Bus interface name the proxy is for"),
                                                        NULL,
                                                        G_PARAM_READABLE |
                                                        G_PARAM_WRITABLE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_NAME |
                                                        G_PARAM_STATIC_BLURB |
                                                        G_PARAM_STATIC_NICK));

  /**
   * GDBusProxy:g-default-timeout:
   *
   * The timeout to use if -1 (specifying default timeout) is passed
   * as @timeout_msec in the g_dbus_proxy_invoke_method() and
   * g_dbus_proxy_invoke_method_sync() functions.
   *
   * This allows applications to set a proxy-wide timeout for all
   * remote method invocations on the proxy. If this property is -1,
   * the default timeout (typically 25 seconds) is used. If set to
   * %G_MAXINT, then no timeout is used.
   */
  g_object_class_install_property (gobject_class,
                                   PROP_G_DEFAULT_TIMEOUT,
                                   g_param_spec_int ("g-default-timeout",
                                                     _("Default Timeout"),
                                                     _("Timeout for remote method invocation"),
                                                     -1,
                                                     G_MAXINT,
                                                     -1,
                                                     G_PARAM_READABLE |
                                                     G_PARAM_WRITABLE |
                                                     G_PARAM_CONSTRUCT |
                                                     G_PARAM_STATIC_NAME |
                                                     G_PARAM_STATIC_BLURB |
                                                     G_PARAM_STATIC_NICK));

  /**
   * GDBusProxy::g-properties-changed:
   * @proxy: The #GDBusProxy emitting the signal.
   * @changed_properties: A #GHashTable containing the properties that changed.
   *
   * Emitted when one or more D-Bus properties on @proxy changes. The cached properties
   * are already replaced when this signal fires.
   */
  signals[PROPERTIES_CHANGED_SIGNAL] = g_signal_new ("g-properties-changed",
                                                     G_TYPE_DBUS_PROXY,
                                                     G_SIGNAL_RUN_LAST,
                                                     G_STRUCT_OFFSET (GDBusProxyClass, properties_changed),
                                                     NULL,
                                                     NULL,
                                                     g_cclosure_marshal_VOID__BOXED,
                                                     G_TYPE_NONE,
                                                     1,
                                                     G_TYPE_HASH_TABLE);

  /**
   * GDBusProxy::g-signal:
   * @proxy: The #GDBusProxy emitting the signal.
   * @sender_name: The sender of the signal or %NULL if the connection is not a bus connection.
   * @signal_name: The name of the signal.
   * @parameters: A #GVariant tuple with parameters for the signal.
   *
   * Emitted when a signal from the remote object and interface that @proxy is for, has been received.
   **/
  signals[SIGNAL_SIGNAL] = g_signal_new ("g-signal",
                                         G_TYPE_DBUS_PROXY,
                                         G_SIGNAL_RUN_LAST,
                                         G_STRUCT_OFFSET (GDBusProxyClass, signal),
                                         NULL,
                                         NULL,
                                         _gdbus_marshal_VOID__STRING_STRING_BOXED,
                                         G_TYPE_NONE,
                                         3,
                                         G_TYPE_STRING,
                                         G_TYPE_STRING,
                                         G_TYPE_VARIANT);


  g_type_class_add_private (klass, sizeof (GDBusProxyPrivate));
}

static void
g_dbus_proxy_init (GDBusProxy *proxy)
{
  proxy->priv = G_TYPE_INSTANCE_GET_PRIVATE (proxy, G_TYPE_DBUS_PROXY, GDBusProxyPrivate);
}

/* ---------------------------------------------------------------------------------------------------- */

/**
 * g_dbus_proxy_get_cached_property_names:
 * @proxy: A #GDBusProxy.
 * @error: Return location for error or %NULL.
 *
 * Gets the names of all cached properties on @proxy.
 *
 * Returns: A %NULL-terminated array of strings or %NULL if @error is set. Free with
 * g_strfreev().
 */
gchar **
g_dbus_proxy_get_cached_property_names (GDBusProxy          *proxy,
                                        GError             **error)
{
  gchar **names;
  GPtrArray *p;
  GHashTableIter iter;
  const gchar *key;

  g_return_val_if_fail (G_IS_DBUS_PROXY (proxy), NULL);

  names = NULL;

  if (proxy->priv->flags & G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES)
    {
      g_set_error (error,
                   G_DBUS_ERROR,
                   G_DBUS_ERROR_FAILED,
                   _("Properties are not available (proxy created with G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES)"));
      goto out;
    }

  p = g_ptr_array_new ();

  g_hash_table_iter_init (&iter, proxy->priv->properties);
  while (g_hash_table_iter_next (&iter, (gpointer) &key, NULL))
    {
      g_ptr_array_add (p, g_strdup (key));
    }
  g_ptr_array_sort (p, (GCompareFunc) g_strcmp0);
  g_ptr_array_add (p, NULL);

  names = (gchar **) g_ptr_array_free (p, FALSE);

 out:
  return names;
}

/**
 * g_dbus_proxy_get_cached_property:
 * @proxy: A #GDBusProxy.
 * @property_name: Property name.
 * @error: Return location for error or %NULL.
 *
 * Looks up the value for a property from the cache. This call does no blocking IO.
 *
 * Normally you will not need to modify the returned variant since it is updated automatically
 * in response to <literal>org.freedesktop.DBus.Properties.PropertiesChanged</literal>
 * D-Bus signals (which also causes #GDBusProxy::g-properties-changed to be emitted).
 *
 * However, for properties for which said D-Bus signal is not emitted, you
 * can catch other signals and modify the returned variant accordingly (remember to emit
 * #GDBusProxy::g-properties-changed yourself).
 *
 * Returns: A reference to the #GVariant instance that holds the value for @property_name or
 * %NULL if @error is set. Free the reference with g_variant_unref().
 */
GVariant *
g_dbus_proxy_get_cached_property (GDBusProxy          *proxy,
                                  const gchar         *property_name,
                                  GError             **error)
{
  GVariant *value;

  g_return_val_if_fail (G_IS_DBUS_PROXY (proxy), NULL);
  g_return_val_if_fail (property_name != NULL, NULL);

  value = NULL;

  if (proxy->priv->flags & G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES)
    {
      g_set_error (error,
                   G_DBUS_ERROR,
                   G_DBUS_ERROR_FAILED,
                   _("Properties are not available (proxy created with G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES)"));
      goto out;
    }

  value = g_hash_table_lookup (proxy->priv->properties, property_name);
  if (value == NULL)
    {
      g_set_error (error,
                   G_DBUS_ERROR,
                   G_DBUS_ERROR_FAILED,
                   _("No property with name %s"),
                   property_name);
      goto out;
    }

  g_variant_ref (value);

 out:

  return value;
}

/* ---------------------------------------------------------------------------------------------------- */

static void
on_signal_received (GDBusConnection  *connection,
                    const gchar      *sender_name,
                    const gchar      *object_path,
                    const gchar      *interface_name,
                    const gchar      *signal_name,
                    GVariant         *parameters,
                    gpointer          user_data)
{
  GDBusProxy *proxy = G_DBUS_PROXY (user_data);

  g_signal_emit (proxy,
                 signals[SIGNAL_SIGNAL],
                 0,
                 sender_name,
                 signal_name,
                 parameters);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
on_properties_changed (GDBusConnection  *connection,
                       const gchar      *sender_name,
                       const gchar      *object_path,
                       const gchar      *interface_name,
                       const gchar      *signal_name,
                       GVariant         *parameters,
                       gpointer          user_data)
{
  GDBusProxy *proxy = G_DBUS_PROXY (user_data);
  GError *error;
  const gchar *interface_name_for_signal;
  GVariantIter iter;
  GVariant *item;
  GHashTable *changed_properties;

  error = NULL;

#if 0 // TODO!
  /* Ignore this signal if properties are not yet available
   *
   * (can happen in the window between subscribing to PropertiesChanged() and until
   *  org.freedesktop.DBus.Properties.GetAll() returns)
   */
  if (!proxy->priv->properties_available)
    goto out;
#endif

  g_variant_get (parameters,
                 "(sa{sv})",
                 &interface_name_for_signal,
                 &iter);

  if (g_strcmp0 (interface_name_for_signal, proxy->priv->interface_name) != 0)
    goto out;

  changed_properties = g_hash_table_new_full (g_str_hash,
                                              g_str_equal,
                                              g_free,
                                              (GDestroyNotify) g_variant_unref);

  while ((item = g_variant_iter_next_value (&iter)))
    {
      const gchar *key;
      GVariant *value;

      g_variant_get (item,
                     "{sv}",
                     &key,
                     &value);

      g_hash_table_insert (proxy->priv->properties,
                           g_strdup (key),
                           value); /* steals value */

      g_hash_table_insert (changed_properties,
                           g_strdup (key),
                           g_variant_ref (value));
    }


  /* emit signal */
  g_signal_emit (proxy, signals[PROPERTIES_CHANGED_SIGNAL], 0, changed_properties);

  g_hash_table_unref (changed_properties);

 out:
  ;
}

/* ---------------------------------------------------------------------------------------------------- */

static void
g_dbus_proxy_constructed (GObject *object)
{
  if (G_OBJECT_CLASS (g_dbus_proxy_parent_class)->constructed != NULL)
    G_OBJECT_CLASS (g_dbus_proxy_parent_class)->constructed (object);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
subscribe_to_signals (GDBusProxy *proxy)
{
  if (!(proxy->priv->flags & G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES))
    {
      /* subscribe to PropertiesChanged() */
      proxy->priv->properties_changed_subscriber_id =
        g_dbus_connection_signal_subscribe (proxy->priv->connection,
                                            proxy->priv->unique_bus_name,
                                            "org.freedesktop.DBus.Properties",
                                            "PropertiesChanged",
                                            proxy->priv->object_path,
                                            proxy->priv->interface_name,
                                            on_properties_changed,
                                            proxy,
                                            NULL);
    }

  if (!(proxy->priv->flags & G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS))
    {
      /* subscribe to all signals for the object */
      proxy->priv->signals_subscriber_id =
        g_dbus_connection_signal_subscribe (proxy->priv->connection,
                                            proxy->priv->unique_bus_name,
                                            proxy->priv->interface_name,
                                            NULL,                        /* member */
                                            proxy->priv->object_path,
                                            NULL,                        /* arg0 */
                                            on_signal_received,
                                            proxy,
                                            NULL);
    }
}

/* ---------------------------------------------------------------------------------------------------- */

static void
process_get_all_reply (GDBusProxy *proxy,
                       GVariant   *result)
{
  GVariantIter iter;
  GVariant *item;

  proxy->priv->properties = g_hash_table_new_full (g_str_hash,
                                                   g_str_equal,
                                                   g_free,
                                                   (GDestroyNotify) g_variant_unref);

  g_variant_iter_init (&iter, g_variant_get_child_value (result, 0));
  while ((item = g_variant_iter_next_value (&iter)) != NULL)
    {
      const gchar *key;
      GVariant *value;

      g_variant_get (item,
                     "{sv}",
                     &key,
                     &value);
      //g_print ("got %s -> %s\n", key, g_variant_markup_print (value, FALSE, 0, 0));

      g_hash_table_insert (proxy->priv->properties,
                           g_strdup (key),
                           value); /* steals value */
    }
}

static gboolean
initable_init (GInitable       *initable,
               GCancellable    *cancellable,
               GError         **error)
{
  GDBusProxy *proxy = G_DBUS_PROXY (initable);
  GVariant *result;
  gboolean ret;

  ret = FALSE;

  if (!(proxy->priv->flags & G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES))
    {
      /* load all properties synchronously */
      result = g_dbus_connection_invoke_method_sync (proxy->priv->connection,
                                                     proxy->priv->unique_bus_name,
                                                     proxy->priv->object_path,
                                                     "org.freedesktop.DBus.Properties",
                                                     "GetAll",
                                                     g_variant_new ("(s)", proxy->priv->interface_name),
                                                     -1,           /* timeout */
                                                     cancellable,
                                                     error);
      if (result == NULL)
        goto out;

      process_get_all_reply (proxy, result);

      g_variant_unref (result);
    }

  subscribe_to_signals (proxy);

  ret = TRUE;

 out:
  return ret;
}

static void
initable_iface_init (GInitableIface *initable_iface)
{
  initable_iface->init = initable_init;
}

/* ---------------------------------------------------------------------------------------------------- */

static void
get_all_cb (GDBusConnection *connection,
            GAsyncResult    *res,
            gpointer         user_data)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (user_data);
  GVariant *result;
  GError *error;

  result = g_dbus_connection_invoke_method_finish (connection,
                                                   res,
                                                   &error);
  if (result == NULL)
    {
      g_simple_async_result_set_from_error (simple, error);
      g_error_free (error);
    }
  else
    {
      g_simple_async_result_set_op_res_gpointer (simple,
                                                 result,
                                                 (GDestroyNotify) g_variant_unref);
    }

  g_simple_async_result_complete_in_idle (simple);
  g_object_unref (simple);
}

static void
async_initable_init_async (GAsyncInitable     *initable,
                           gint                io_priority,
                           GCancellable       *cancellable,
                           GAsyncReadyCallback callback,
                           gpointer            user_data)
{
  GDBusProxy *proxy = G_DBUS_PROXY (initable);
  GSimpleAsyncResult *simple;

  simple = g_simple_async_result_new (G_OBJECT (proxy),
                                      callback,
                                      user_data,
                                      NULL);

  if (!(proxy->priv->flags & G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES))
    {
      /* load all properties asynchronously */
      g_dbus_connection_invoke_method (proxy->priv->connection,
                                       proxy->priv->unique_bus_name,
                                       proxy->priv->object_path,
                                       "org.freedesktop.DBus.Properties",
                                       "GetAll",
                                       g_variant_new ("(s)", proxy->priv->interface_name),
                                       -1,           /* timeout */
                                       cancellable,
                                       (GAsyncReadyCallback) get_all_cb,
                                       simple);
    }
  else
    {
      g_simple_async_result_complete_in_idle (simple);
      g_object_unref (simple);
    }
}

static gboolean
async_initable_init_finish (GAsyncInitable  *initable,
                            GAsyncResult    *res,
                            GError         **error)
{
  GDBusProxy *proxy = G_DBUS_PROXY (initable);
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (res);
  GVariant *result;
  gboolean ret;

  ret = FALSE;

  result = g_simple_async_result_get_op_res_gpointer (simple);
  if (result == NULL)
    goto out;

  process_get_all_reply (proxy, result);

  subscribe_to_signals (proxy);

  ret = TRUE;

 out:
  return ret;
}

static void
async_initable_iface_init (GAsyncInitableIface *async_initable_iface)
{
  async_initable_iface->init_async = async_initable_init_async;
  async_initable_iface->init_finish = async_initable_init_finish;
}

/* ---------------------------------------------------------------------------------------------------- */

/**
 * g_dbus_proxy_new:
 * @connection: A #GDBusConnection.
 * @object_type: Either #G_TYPE_DBUS_PROXY or the #GType for the #GDBusProxy<!-- -->-derived type of proxy to create.
 * @flags: Flags used when constructing the proxy.
 * @unique_bus_name: A unique bus name or %NULL if @connection is not a message bus connection.
 * @object_path: An object path.
 * @interface_name: A D-Bus interface name.
 * @cancellable: A #GCancellable or %NULL.
 * @callback: Callback function to invoke when the proxy is ready.
 * @user_data: User data to pass to @callback.
 *
 * Creates a proxy for accessing @interface_name on the remote object at @object_path
 * owned by @unique_bus_name at @connection and asynchronously loads D-Bus properties unless the
 * #G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES flag is used. Connect to the
 * #GDBusProxy::g-properties-changed signal to get notified about property changes.
 *
 * If the #G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS flag is not set, also sets up
 * match rules for signals. Connect to the #GDBusProxy::g-signal signal
 * to handle signals from the remote object.
 *
 * This is a failable asynchronous constructor - when the proxy is
 * ready, @callback will be invoked and you can use
 * g_dbus_proxy_new_finish() to get the result.
 *
 * See g_dbus_proxy_new_sync() and for a synchronous version of this constructor.
 **/
void
g_dbus_proxy_new (GDBusConnection     *connection,
                  GType                object_type,
                  GDBusProxyFlags      flags,
                  const gchar         *unique_bus_name,
                  const gchar         *object_path,
                  const gchar         *interface_name,
                  GCancellable        *cancellable,
                  GAsyncReadyCallback  callback,
                  gpointer             user_data)
{
  g_return_if_fail (G_IS_DBUS_CONNECTION (connection));
  g_return_if_fail (g_type_is_a (object_type, G_TYPE_DBUS_PROXY));
  /* TODO: check that unique_bus_name is unique */
  g_return_if_fail ((g_dbus_connection_get_bus_type (connection) == G_BUS_TYPE_NONE && unique_bus_name == NULL) ||
                    (g_dbus_connection_get_bus_type (connection) != G_BUS_TYPE_NONE || unique_bus_name != NULL));
  g_return_if_fail (object_path != NULL);
  g_return_if_fail (interface_name);

  g_async_initable_new_async (object_type,
                              G_PRIORITY_DEFAULT,
                              cancellable,
                              callback,
                              user_data,
                              "g-flags", flags,
                              "g-unique-bus-name", unique_bus_name,
                              "g-connection", connection,
                              "g-object-path", object_path,
                              "g-interface-name", interface_name,
                              NULL);
}

/**
 * g_dbus_proxy_new_finish:
 * @res: A #GAsyncResult obtained from the #GAsyncReadyCallback function passed to g_dbus_proxy_new().
 * @error: Return location for error or %NULL.
 *
 * Finishes creating a #GDBusProxy.
 *
 * Returns: A #GDBusProxy or %NULL if @error is set. Free with g_object_unref().
 **/
GDBusProxy *
g_dbus_proxy_new_finish (GAsyncResult  *res,
                         GError       **error)
{
  GObject *object;
  GObject *source_object;

  source_object = g_async_result_get_source_object (res);
  g_assert (source_object != NULL);

  object = g_async_initable_new_finish (G_ASYNC_INITABLE (source_object),
                                        res,
                                        error);
  g_object_unref (source_object);

  if (object != NULL)
    return G_DBUS_PROXY (object);
  else
    return NULL;
}


/* ---------------------------------------------------------------------------------------------------- */

/**
 * g_dbus_proxy_new_sync:
 * @connection: A #GDBusConnection.
 * @object_type: Either #G_TYPE_DBUS_PROXY or the #GType for the #GDBusProxy<!-- -->-derived type of proxy to create.
 * @flags: Flags used when constructing the proxy.
 * @unique_bus_name: A unique bus name or %NULL if @connection is not a message bus connection.
 * @object_path: An object path.
 * @interface_name: A D-Bus interface name.
 * @cancellable: A #GCancellable or %NULL.
 * @error: Return location for error or %NULL.
 *
 * Creates a proxy for accessing @interface_name on the remote object at @object_path
 * owned by @unique_bus_name at @connection and synchronously loads D-Bus properties unless the
 * #G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES flag is used.
 *
 * If the #G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS flag is not set, also sets up
 * match rules for signals. Connect to the #GDBusProxy::g-signal signal
 * to handle signals from the remote object.
 *
 * This is a synchronous failable constructor. See g_dbus_proxy_new()
 * and g_dbus_proxy_new_finish() for the asynchronous version.
 *
 * Returns: A #GDBusProxy or %NULL if error is set. Free with g_object_unref().
 **/
GDBusProxy *
g_dbus_proxy_new_sync (GDBusConnection     *connection,
                       GType                object_type,
                       GDBusProxyFlags      flags,
                       const gchar         *unique_bus_name,
                       const gchar         *object_path,
                       const gchar         *interface_name,
                       GCancellable        *cancellable,
                       GError             **error)
{
  GInitable *initable;

  g_return_val_if_fail (G_IS_DBUS_CONNECTION (connection), NULL);
  g_return_val_if_fail (g_type_is_a (object_type, G_TYPE_DBUS_PROXY), NULL);
  g_return_val_if_fail ((g_dbus_connection_get_bus_type (connection) == G_BUS_TYPE_NONE && unique_bus_name == NULL) ||
                        (g_dbus_connection_get_bus_type (connection) != G_BUS_TYPE_NONE || unique_bus_name != NULL),
                        NULL);
  g_return_val_if_fail (object_path != NULL, NULL);
  g_return_val_if_fail (interface_name, NULL);

  initable = g_initable_new (object_type,
                             cancellable,
                             error,
                             "g-flags", flags,
                             "g-unique-bus-name", unique_bus_name,
                             "g-connection", connection,
                             "g-object-path", object_path,
                             "g-interface-name", interface_name,
                             NULL);
  if (initable != NULL)
    return G_DBUS_PROXY (initable);
  else
    return NULL;
}

/* ---------------------------------------------------------------------------------------------------- */

/**
 * g_dbus_proxy_get_connection:
 * @proxy: A #GDBusProxy.
 *
 * Gets the connection @proxy is for.
 *
 * Returns: A #GDBusConnection owned by @proxy. Do not free.
 **/
GDBusConnection *
g_dbus_proxy_get_connection (GDBusProxy *proxy)
{
  g_return_val_if_fail (G_IS_DBUS_PROXY (proxy), NULL);
  return proxy->priv->connection;
}

/**
 * g_dbus_proxy_get_flags:
 * @proxy: A #GDBusProxy.
 *
 * Gets the flags that @proxy was constructed with.
 *
 * Returns: Flags from the #GDBusProxyFlags enumeration.
 **/
GDBusProxyFlags
g_dbus_proxy_get_flags (GDBusProxy *proxy)
{
  g_return_val_if_fail (G_IS_DBUS_PROXY (proxy), 0);
  return proxy->priv->flags;
}

/**
 * g_dbus_proxy_get_unique_bus_name:
 * @proxy: A #GDBusProxy.
 *
 * Gets the unique bus name @proxy is for.
 *
 * Returns: A string owned by @proxy. Do not free.
 **/
const gchar *
g_dbus_proxy_get_unique_bus_name (GDBusProxy *proxy)
{
  g_return_val_if_fail (G_IS_DBUS_PROXY (proxy), NULL);
  return proxy->priv->unique_bus_name;
}

/**
 * g_dbus_proxy_get_object_path:
 * @proxy: A #GDBusProxy.
 *
 * Gets the object path @proxy is for.
 *
 * Returns: A string owned by @proxy. Do not free.
 **/
const gchar *
g_dbus_proxy_get_object_path (GDBusProxy *proxy)
{
  g_return_val_if_fail (G_IS_DBUS_PROXY (proxy), NULL);
  return proxy->priv->object_path;
}

/**
 * g_dbus_proxy_get_interface_name:
 * @proxy: A #GDBusProxy.
 *
 * Gets the D-Bus interface name @proxy is for.
 *
 * Returns: A string owned by @proxy. Do not free.
 **/
const gchar *
g_dbus_proxy_get_interface_name (GDBusProxy *proxy)
{
  g_return_val_if_fail (G_IS_DBUS_PROXY (proxy), NULL);
  return proxy->priv->interface_name;
}

/**
 * g_dbus_proxy_get_default_timeout:
 * @proxy: A #GDBusProxy.
 *
 * Gets the timeout to use if -1 (specifying default timeout) is
 * passed as @timeout_msec in the g_dbus_proxy_invoke_method() and
 * g_dbus_proxy_invoke_method_sync() functions.
 *
 * See the #GDBusProxy:g-default-timeout property for more details.
 *
 * Returns: Timeout to use for @proxy.
 */
gint
g_dbus_proxy_get_default_timeout (GDBusProxy *proxy)
{
  g_return_val_if_fail (G_IS_DBUS_PROXY (proxy), -1);
  return proxy->priv->timeout_msec;
}

/**
 * g_dbus_proxy_set_default_timeout:
 * @proxy: A #GDBusProxy.
 * @timeout_msec: Timeout in milliseconds.
 *
 * Sets the timeout to use if -1 (specifying default timeout) is
 * passed as @timeout_msec in the g_dbus_proxy_invoke_method() and
 * g_dbus_proxy_invoke_method_sync() functions.
 *
 * See the #GDBusProxy:g-default-timeout property for more details.
 */
void
g_dbus_proxy_set_default_timeout (GDBusProxy *proxy,
                                  gint        timeout_msec)
{
  g_return_if_fail (G_IS_DBUS_PROXY (proxy));
  g_return_if_fail (timeout_msec >= -1);

  /* TODO: locking? */
  if (proxy->priv->timeout_msec != timeout_msec)
    {
      proxy->priv->timeout_msec = timeout_msec;
      g_object_notify (G_OBJECT (proxy), "g-default-timeout");
    }
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
maybe_split_method_name (const gchar   *method_name,
                         gchar        **out_interface_name,
                         const gchar  **out_method_name)
{
  gboolean was_split;

  was_split = FALSE;
  g_assert (out_interface_name != NULL);
  g_assert (out_method_name != NULL);
  *out_interface_name = NULL;
  *out_method_name = NULL;

  if (strchr (method_name, '.') != NULL)
    {
      gchar *p;
      gchar *last_dot;

      p = g_strdup (method_name);
      last_dot = strrchr (p, '.');
      *last_dot = '\0';

      *out_interface_name = p;
      *out_method_name = last_dot + 1;

      was_split = TRUE;
    }

  return was_split;
}


static void
reply_cb (GDBusConnection *connection,
          GAsyncResult    *res,
          gpointer         user_data)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (user_data);
  GVariant *value;
  GError *error;

  error = NULL;
  value = g_dbus_connection_invoke_method_finish (connection,
                                                  res,
                                                  &error);
  if (error != NULL)
    {
      g_simple_async_result_set_from_error (simple,
                                            error);
      g_error_free (error);
    }
  else
    {
      g_simple_async_result_set_op_res_gpointer (simple,
                                                 value,
                                                 (GDestroyNotify) g_variant_unref);
    }

  /* no need to complete in idle since the method GDBusConnection already does */
  g_simple_async_result_complete (simple);
}

/**
 * g_dbus_proxy_invoke_method:
 * @proxy: A #GDBusProxy.
 * @method_name: Name of method to invoke.
 * @parameters: A #GVariant tuple with parameters for the signal or %NULL if not passing parameters.
 * @timeout_msec: The timeout in milliseconds or -1 to use the proxy default timeout.
 * @cancellable: A #GCancellable or %NULL.
 * @callback: A #GAsyncReadyCallback to call when the request is satisfied or %NULL if you don't
 * care about the result of the method invocation.
 * @user_data: The data to pass to @callback.
 *
 * Asynchronously invokes the @method_name method on @proxy.
 *
 * If @method_name contains any dots, then @name is split into interface and
 * method name parts. This allows using @proxy for invoking methods on
 * other interfaces.
 *
 * If the #GDBusConnection associated with @proxy is disconnected then
 * the operation will fail with %G_DBUS_ERROR_DISCONNECTED. If
 * @cancellable is canceled, the operation will fail with
 * %G_DBUS_ERROR_CANCELLED. If @parameters contains a value not
 * compatible with the D-Bus protocol, the operation fails with
 * %G_DBUS_ERROR_CONVERSION_FAILED.
 *
 * This is an asynchronous method. When the operation is finished, @callback will be invoked
 * in the <link linkend="g-main-context-push-thread-default">thread-default main loop</link>
 * of the thread you are calling this method from. You can then call
 * g_dbus_proxy_invoke_method_finish() to get the result of the operation.
 * See g_dbus_proxy_invoke_method_sync() for the
 * synchronous version of this method.
 */
void
g_dbus_proxy_invoke_method (GDBusProxy          *proxy,
                            const gchar         *method_name,
                            GVariant            *parameters,
                            gint                 timeout_msec,
                            GCancellable        *cancellable,
                            GAsyncReadyCallback  callback,
                            gpointer             user_data)
{
  GSimpleAsyncResult *simple;
  gboolean was_split;
  gchar *split_interface_name;
  const gchar *split_method_name;

  g_return_if_fail (G_IS_DBUS_PROXY (proxy));
  g_return_if_fail (method_name != NULL);

  simple = g_simple_async_result_new (G_OBJECT (proxy),
                                      callback,
                                      user_data,
                                      g_dbus_proxy_invoke_method);

  was_split = maybe_split_method_name (method_name, &split_interface_name, &split_method_name);

  g_dbus_connection_invoke_method (proxy->priv->connection,
                                   proxy->priv->unique_bus_name,
                                   proxy->priv->object_path,
                                   was_split ? split_interface_name : proxy->priv->interface_name,
                                   was_split ? split_method_name : method_name,
                                   parameters,
                                   timeout_msec == -1 ? proxy->priv->timeout_msec : timeout_msec,
                                   cancellable,
                                   (GAsyncReadyCallback) reply_cb,
                                   simple);

  g_free (split_interface_name);
}

/**
 * g_dbus_proxy_invoke_method_finish:
 * @proxy: A #GDBusProxy.
 * @res: A #GAsyncResult obtained from the #GAsyncReadyCallback passed to g_dbus_proxy_invoke_method().
 * @error: Return location for error or %NULL.
 *
 * Finishes an operation started with g_dbus_proxy_invoke_method().
 *
 * Returns: %NULL if @error is set. Otherwise a #GVariant tuple with
 * return values. Free with g_variant_unref().
 */
GVariant *
g_dbus_proxy_invoke_method_finish (GDBusProxy    *proxy,
                                   GAsyncResult  *res,
                                   GError       **error)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (res);
  GVariant *value;

  g_return_val_if_fail (G_IS_DBUS_PROXY (proxy), NULL);
  g_return_val_if_fail (res != NULL, NULL);

  g_warn_if_fail (g_simple_async_result_get_source_tag (simple) == g_dbus_proxy_invoke_method);

  value = NULL;

  if (g_simple_async_result_propagate_error (simple, error))
    goto out;

  value = g_simple_async_result_get_op_res_gpointer (simple);

 out:
  return value;
}

/**
 * g_dbus_proxy_invoke_method_sync:
 * @proxy: A #GDBusProxy.
 * @method_name: Name of method to invoke.
 * @parameters: A #GVariant tuple with parameters for the signal or %NULL if not passing parameters.
 * @timeout_msec: The timeout in milliseconds or -1 to use the proxy default timeout.
 * @cancellable: A #GCancellable or %NULL.
 * @error: Return location for error or %NULL.
 *
 * Synchronously invokes the @method_name method on @proxy.
 *
 * If @method_name contains any dots, then @name is split into interface and
 * method name parts. This allows using @proxy for invoking methods on
 * other interfaces.
 *
 * If the #GDBusConnection associated with @proxy is disconnected then
 * the operation will fail with %G_DBUS_ERROR_DISCONNECTED. If
 * @cancellable is canceled, the operation will fail with
 * %G_DBUS_ERROR_CANCELLED. If @parameters contains a value not
 * compatible with the D-Bus protocol, the operation fails with
 * %G_DBUS_ERROR_CONVERSION_FAILED.
 *
 * The calling thread is blocked until a reply is received. See
 * g_dbus_proxy_invoke_method() for the asynchronous version of this
 * method.
 *
 * Returns: %NULL if @error is set. Otherwise a #GVariant tuple with
 * return values. Free with g_variant_unref().
 */
GVariant *
g_dbus_proxy_invoke_method_sync (GDBusProxy     *proxy,
                                 const gchar    *method_name,
                                 GVariant       *parameters,
                                 gint            timeout_msec,
                                 GCancellable   *cancellable,
                                 GError        **error)
{
  GVariant *ret;
  gboolean was_split;
  gchar *split_interface_name;
  const gchar *split_method_name;

  g_return_val_if_fail (G_IS_DBUS_PROXY (proxy), NULL);
  g_return_val_if_fail (method_name != NULL, NULL);

  was_split = maybe_split_method_name (method_name, &split_interface_name, &split_method_name);

  ret = g_dbus_connection_invoke_method_sync (proxy->priv->connection,
                                              proxy->priv->unique_bus_name,
                                              proxy->priv->object_path,
                                              was_split ? split_interface_name : proxy->priv->interface_name,
                                              was_split ? split_method_name : method_name,
                                              parameters,
                                              timeout_msec == -1 ? proxy->priv->timeout_msec : timeout_msec,
                                              cancellable,
                                              error);

  g_free (split_interface_name);

  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */
