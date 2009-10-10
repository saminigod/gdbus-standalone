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

#include "gdbusconnection.h"
#include "gdbusconnection-lowlevel.h"
#include "gdbusmainloop-lowlevel.h"
#include "gdbusctypemapping-lowlevel.h"
#include "gdbuserror.h"
#include "gdbusenumtypes.h"
#include "gdbusprivate.h"

#include "gdbusalias.h"

/**
 * SECTION:gdbusconnection
 * @short_description: D-Bus Connections
 * @include: gdbus/gdbus.h
 *
 * <para><note>
 * This class is rarely used directly. If you are writing an application, it is often
 * easier to use the g_bus_own_name() or g_bus_watch_name() APIs.
 * </note></para>
 * #GDBusConnection is a thin wrapper class for the #DBusConnection
 * type that integrates with the GLib type system.
 *
 * TODO: stuff about caching unix_process_id etc. when we add that.
 */

struct _GDBusConnectionPrivate
{
  /* construct properties */
  DBusConnection *dbus_1_connection;
  GBusType        bus_type;
  gboolean        is_private;

  gboolean is_initialized;
  GError *initialization_error;

  /* unfortunately there is no dbus_connection_get_exit_on_disconnect() so we need to track this ourselves */
  gboolean exit_on_disconnect;

  /* Maps used for signal subscription */
  GHashTable *map_rule_to_signal_data;
  GHashTable *map_id_to_signal_data;
  GHashTable *map_sender_to_signal_data_array;
};

enum
{
  DISCONNECTED_SIGNAL,
  LAST_SIGNAL,
};

enum
{
  PROP_0,
  PROP_BUS_TYPE,
  PROP_IS_PRIVATE,
  PROP_UNIQUE_NAME,
  PROP_IS_DISCONNECTED,
  PROP_EXIT_ON_DISCONNECT,
};

static void distribute_signals (GDBusConnection *connection,
                                DBusMessage     *message);

static void purge_all_signal_subscriptions (GDBusConnection *connection);

G_LOCK_DEFINE_STATIC (connection_lock);

static GDBusConnection *the_session_bus = NULL;
static GDBusConnection *the_system_bus = NULL;

static GObject *g_dbus_connection_constructor (GType                  type,
                                               guint                  n_construct_properties,
                                               GObjectConstructParam *construct_properties);

static DBusHandlerResult
filter_function (DBusConnection *dbus_1_connection,
                 DBusMessage    *message,
                 void           *user_data);

static guint signals[LAST_SIGNAL] = { 0 };

static void g_dbus_connection_set_dbus_1_connection (GDBusConnection *connection,
                                                     DBusConnection  *dbus_1_connection);

static void initable_iface_init       (GInitableIface *initable_iface);
static void async_initable_iface_init (GAsyncInitableIface *async_initable_iface);

G_DEFINE_TYPE_WITH_CODE (GDBusConnection, g_dbus_connection, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE, initable_iface_init)
                         G_IMPLEMENT_INTERFACE (G_TYPE_ASYNC_INITABLE, async_initable_iface_init)
                         );

static void
g_dbus_connection_dispose (GObject *object)
{
  GDBusConnection *connection = G_DBUS_CONNECTION (object);

  G_LOCK (connection_lock);
  if (connection == the_session_bus)
    {
      the_session_bus = NULL;
    }
  else if (connection == the_system_bus)
    {
      the_system_bus = NULL;
    }
  G_UNLOCK (connection_lock);

  if (G_OBJECT_CLASS (g_dbus_connection_parent_class)->dispose != NULL)
    G_OBJECT_CLASS (g_dbus_connection_parent_class)->dispose (object);
}

static void
g_dbus_connection_finalize (GObject *object)
{
  GDBusConnection *connection = G_DBUS_CONNECTION (object);

  g_dbus_connection_set_dbus_1_connection (connection, NULL);

  if (connection->priv->initialization_error != NULL)
    g_error_free (connection->priv->initialization_error);

  purge_all_signal_subscriptions (connection);
  g_hash_table_unref (connection->priv->map_rule_to_signal_data);
  g_hash_table_unref (connection->priv->map_id_to_signal_data);
  g_hash_table_unref (connection->priv->map_sender_to_signal_data_array);

  if (G_OBJECT_CLASS (g_dbus_connection_parent_class)->finalize != NULL)
    G_OBJECT_CLASS (g_dbus_connection_parent_class)->finalize (object);
}

static void
g_dbus_connection_get_property (GObject    *object,
                                guint       prop_id,
                                GValue     *value,
                                GParamSpec *pspec)
{
  GDBusConnection *connection = G_DBUS_CONNECTION (object);

  switch (prop_id)
    {
    case PROP_BUS_TYPE:
      g_value_set_enum (value, g_dbus_connection_get_bus_type (connection));
      break;

    case PROP_IS_PRIVATE:
      g_value_set_boolean (value, g_dbus_connection_get_is_private (connection));
      break;

    case PROP_UNIQUE_NAME:
      g_value_set_string (value, g_dbus_connection_get_unique_name (connection));
      break;

    case PROP_IS_DISCONNECTED:
      g_value_set_boolean (value, g_dbus_connection_get_is_disconnected (connection));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
g_dbus_connection_set_property (GObject      *object,
                                guint         prop_id,
                                const GValue *value,
                                GParamSpec   *pspec)
{
  GDBusConnection *connection = G_DBUS_CONNECTION (object);

  switch (prop_id)
    {
    case PROP_BUS_TYPE:
      connection->priv->bus_type = g_value_get_enum (value);
      break;

    case PROP_IS_PRIVATE:
      connection->priv->is_private = g_value_get_boolean (value);
      break;

    case PROP_EXIT_ON_DISCONNECT:
      g_dbus_connection_set_exit_on_disconnect (connection, g_value_get_boolean (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
g_dbus_connection_class_init (GDBusConnectionClass *klass)
{
  GObjectClass *gobject_class;

  g_type_class_add_private (klass, sizeof (GDBusConnectionPrivate));

  gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->constructor  = g_dbus_connection_constructor;
  gobject_class->finalize     = g_dbus_connection_finalize;
  gobject_class->dispose      = g_dbus_connection_dispose;
  gobject_class->set_property = g_dbus_connection_set_property;
  gobject_class->get_property = g_dbus_connection_get_property;

  /**
   * GDBusConnection:bus-type:
   *
   * When constructing an object, set this to the type of the message bus
   * the connection is for or #G_BUS_TYPE_NONE if the connection is not
   * a message bus connection.
   * This property is ignored if #GDBusConnection:dbus-1-connection is set upon construction.
   *
   * When reading, this property is never #G_BUS_TYPE_STARTER - if #G_BUS_TYPE_STARTER
   * was passed as a construction property, then this property will be either #G_BUS_TYPE_SESSION
   * or #G_BUS_TYPE_SYSTEM depending on what message bus activated the process.
   */
  g_object_class_install_property (gobject_class,
                                   PROP_BUS_TYPE,
                                   g_param_spec_enum ("bus-type",
                                                      _("bus-type"),
                                                      _("The type of message bus the connection is for"),
                                                      G_TYPE_BUS_TYPE,
                                                      G_BUS_TYPE_NONE,
                                                      G_PARAM_READABLE |
                                                      G_PARAM_WRITABLE |
                                                      G_PARAM_CONSTRUCT_ONLY |
                                                      G_PARAM_STATIC_NAME |
                                                      G_PARAM_STATIC_BLURB |
                                                      G_PARAM_STATIC_NICK));

  /**
   * GDBusConnection:is-private:
   *
   * When constructing an object and #GDBusConnection:bus-type is set to something
   * other than #G_BUS_TYPE_NONE, specifies whether the connection to the requested
   * message bus should be a private connection.
   * This property is ignored if #GDBusConnection:dbus-1-connection is set upon construction.
   *
   * When reading, specifies if connection to the message bus is
   * private or shared with others.
   */
  g_object_class_install_property (gobject_class,
                                   PROP_IS_PRIVATE,
                                   g_param_spec_boolean ("is-private",
                                                         _("is-private"),
                                                         _("Whether the connection to the message bus is private"),
                                                         FALSE,
                                                         G_PARAM_READABLE |
                                                         G_PARAM_WRITABLE |
                                                         G_PARAM_CONSTRUCT_ONLY |
                                                         G_PARAM_STATIC_NAME |
                                                         G_PARAM_STATIC_BLURB |
                                                         G_PARAM_STATIC_NICK));

  /**
   * GDBusConnection:unique-name:
   *
   * The unique name as assigned by the message bus or %NULL if the
   * connection is not open or not a message bus connection.
   */
  g_object_class_install_property (gobject_class,
                                   PROP_UNIQUE_NAME,
                                   g_param_spec_string ("unique-name",
                                                        _("unique-name"),
                                                        _("Unique name of bus connection"),
                                                        NULL,
                                                        G_PARAM_READABLE |
                                                        G_PARAM_STATIC_NAME |
                                                        G_PARAM_STATIC_BLURB |
                                                        G_PARAM_STATIC_NICK));

  /**
   * GDBusConnection:is-disconnected:
   *
   * A boolean specifying whether the connection has been disconnected.
   */
  g_object_class_install_property (gobject_class,
                                   PROP_IS_DISCONNECTED,
                                   g_param_spec_boolean ("is-disconnected",
                                                         _("is-disconnected"),
                                                         _("Whether the connection has been disconnected"),
                                                         FALSE,
                                                         G_PARAM_READABLE |
                                                         G_PARAM_STATIC_NAME |
                                                         G_PARAM_STATIC_BLURB |
                                                         G_PARAM_STATIC_NICK));

  /**
   * GDBusConnection:exit-on-disconnect:
   *
   * A boolean specifying whether _exit() should be called when the
   * connection has been disconnected.
   */
  g_object_class_install_property (gobject_class,
                                   PROP_EXIT_ON_DISCONNECT,
                                   g_param_spec_boolean ("exit-on-disconnect",
                                                         _("exit-on-disconnect"),
                                                         _("Whether _exit() is called when the connection has been disconnected"),
                                                         TRUE,
                                                         G_PARAM_WRITABLE |
                                                         G_PARAM_CONSTRUCT |
                                                         G_PARAM_STATIC_NAME |
                                                         G_PARAM_STATIC_BLURB |
                                                         G_PARAM_STATIC_NICK));


  /**
   * GDBusConnection::disconnected:
   * @connection: The #GDBusConnection emitting the signal.
   *
   * Emitted when the connection has been disconnected. You should
   * give up your reference to @connection when receiving this signal.
   *
   * You are guaranteed that this signal is emitted only once.
   **/
  signals[DISCONNECTED_SIGNAL] = g_signal_new ("disconnected",
                                               G_TYPE_DBUS_CONNECTION,
                                               G_SIGNAL_RUN_LAST,
                                               G_STRUCT_OFFSET (GDBusConnectionClass, disconnected),
                                               NULL,
                                               NULL,
                                               g_cclosure_marshal_VOID__VOID,
                                               G_TYPE_NONE,
                                               0);
}

static void
g_dbus_connection_init (GDBusConnection *connection)
{
  connection->priv = G_TYPE_INSTANCE_GET_PRIVATE (connection, G_TYPE_DBUS_CONNECTION, GDBusConnectionPrivate);

  connection->priv->map_rule_to_signal_data = g_hash_table_new (g_str_hash,
                                                                g_str_equal);
  connection->priv->map_id_to_signal_data = g_hash_table_new (g_direct_hash,
                                                              g_direct_equal);
  connection->priv->map_sender_to_signal_data_array = g_hash_table_new_full (g_str_hash,
                                                                             g_str_equal,
                                                                             g_free,
                                                                             NULL);
}

/**
 * g_dbus_connection_get_bus_type:
 * @connection: A #GDBusConnection.
 *
 * Gets the type of message bus connection, if any.
 *
 * This will never return #G_BUS_TYPE_STARTER. If
 * #G_BUS_TYPE_STARTER was passed to g_dbus_connection_bus_get()
 * then the return value will be either #G_BUS_TYPE_SESSION or
 * #G_BUS_TYPE_SYSTEM depending on what bus started the
 * process.
 *
 * Returns: Type type of the message bus the connection is for or
 * #G_BUS_TYPE_NONE if the connection is not to a message
 * bus.
 **/
GBusType
g_dbus_connection_get_bus_type (GDBusConnection *connection)
{
  g_return_val_if_fail (G_IS_DBUS_CONNECTION (connection), G_BUS_TYPE_NONE);

  return connection->priv->bus_type;
}


/**
 * g_dbus_connection_get_is_disconnected:
 * @connection: A #GDBusConnection.
 *
 * Gets whether a connection has been disconnected.
 *
 * Returns: %TRUE if the connection is open, %FALSE otherwise.
 **/
gboolean
g_dbus_connection_get_is_disconnected (GDBusConnection *connection)
{
  g_return_val_if_fail (G_IS_DBUS_CONNECTION (connection), FALSE);

  return connection->priv->dbus_1_connection == NULL;
}

/**
 * g_dbus_connection_get_is_private:
 * @connection: A #GDBusConnection.
 *
 * Gets whether the connection is private.
 *
 * Returns: %TRUE if the connection is private, %FALSE otherwise.
 **/
gboolean
g_dbus_connection_get_is_private (GDBusConnection *connection)
{
  g_return_val_if_fail (G_IS_DBUS_CONNECTION (connection), FALSE);

  return connection->priv->is_private;
}

/**
 * g_dbus_connection_get_dbus_1_connection:
 * @connection: A #GDBusConnection.
 *
 * <para><note>
 * This function is marked as unstable API. You must include <literal>gdbus/gdbus-lowlevel.h</literal> to use it.
 * </note></para>
 *
 * Gets the underlying #DBusConnection object for @connection.
 *
 * Returns: %NULL if the connection has been disconnected, otherwise a
 * #DBusConnection object owned by @connection.
 **/
DBusConnection *
g_dbus_connection_get_dbus_1_connection (GDBusConnection *connection)
{
  g_return_val_if_fail (G_IS_DBUS_CONNECTION (connection), NULL);

  return connection->priv->dbus_1_connection;
}

/* ---------------------------------------------------------------------------------------------------- */

#define PRINT_MESSAGE(message)                          \
  do {                                                  \
    const gchar *message_type;                          \
    switch (dbus_message_get_type (message))            \
      {                                                 \
      case DBUS_MESSAGE_TYPE_METHOD_CALL:               \
        message_type = "method_call";                   \
        break;                                          \
      case DBUS_MESSAGE_TYPE_METHOD_RETURN:             \
        message_type = "method_return";                 \
        break;                                          \
      case DBUS_MESSAGE_TYPE_ERROR:                     \
        message_type = "error";                         \
        break;                                          \
      case DBUS_MESSAGE_TYPE_SIGNAL:                    \
        message_type = "signal";                        \
        break;                                          \
      case DBUS_MESSAGE_TYPE_INVALID:                   \
        message_type = "invalid";                       \
        break;                                          \
      default:                                          \
        message_type = "unknown";                       \
        break;                                          \
      }                                                 \
    g_print ("new message:\n"                           \
             " type:         %s\n"                      \
             " sender:       %s\n"                      \
             " destination:  %s\n"                      \
             " path:         %s\n"                      \
             " interface:    %s\n"                      \
             " member:       %s\n",                     \
             message_type,                              \
             dbus_message_get_sender (message),         \
             dbus_message_get_destination (message),    \
             dbus_message_get_path (message),           \
             dbus_message_get_interface (message),      \
             dbus_message_get_member (message));        \
  } while (FALSE)

static void
process_message (GDBusConnection *connection,
                 DBusMessage *message)
{
  DBusError dbus_error;

  //g_debug ("in filter_function for dbus_1_connection %p", dbus_1_connection);
  //PRINT_MESSAGE (message);

  dbus_error_init (&dbus_error);

  /* check if we are disconnected from the bus */
  if (dbus_message_is_signal (message,
                              DBUS_INTERFACE_LOCAL,
                              "Disconnected") &&
      dbus_message_get_sender (message) == NULL &&
      dbus_message_get_destination (message) == NULL &&
      g_strcmp0 (dbus_message_get_path (message), DBUS_PATH_LOCAL) == 0)
    {
      if (connection->priv->dbus_1_connection != NULL)
        {
          g_dbus_connection_set_dbus_1_connection (connection, NULL);

          g_object_notify (G_OBJECT (connection), "is-disconnected");
          g_signal_emit (connection, signals[DISCONNECTED_SIGNAL], 0);
        }
    }

  /* distribute to signal subscribers */
  distribute_signals (connection, message);
}

static DBusHandlerResult
filter_function (DBusConnection *dbus_1_connection,
                 DBusMessage    *message,
                 void           *user_data)
{
  GDBusConnection *connection = G_DBUS_CONNECTION (user_data);

  //PRINT_MESSAGE (message);
  process_message (connection, message);

  return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static void
g_dbus_connection_set_dbus_1_connection (GDBusConnection *connection,
                                         DBusConnection  *dbus_1_connection)
{
  g_return_if_fail (G_IS_DBUS_CONNECTION (connection));

  if (connection->priv->dbus_1_connection != NULL)
    {
      dbus_connection_remove_filter (connection->priv->dbus_1_connection,
                                     filter_function,
                                     connection);
      g_dbus_unintegrate_dbus_1_connection (connection->priv->dbus_1_connection);
      if (connection->priv->is_private)
        {
          dbus_connection_close (connection->priv->dbus_1_connection);
        }
      else
        {
          /* shared connections must not be closed */
        }
      dbus_connection_unref (connection->priv->dbus_1_connection);
    }

  if (dbus_1_connection != NULL)
    {
      connection->priv->dbus_1_connection = dbus_connection_ref (dbus_1_connection);
      g_dbus_integrate_dbus_1_connection (connection->priv->dbus_1_connection, NULL);
      if (!dbus_connection_add_filter (connection->priv->dbus_1_connection,
                                       filter_function,
                                       connection,
                                       NULL))
        _g_dbus_oom ();
      dbus_connection_set_exit_on_disconnect (connection->priv->dbus_1_connection,
                                              connection->priv->exit_on_disconnect);
    }
  else
    {
      connection->priv->dbus_1_connection = NULL;
    }
}

/* ---------------------------------------------------------------------------------------------------- */

static GObject *
g_dbus_connection_constructor (GType                  type,
                               guint                  n_construct_properties,
                               GObjectConstructParam *construct_properties)
{
  GDBusConnection **singleton;
  gboolean is_private;
  GObject *object;
  guint n;

  object = NULL;
  singleton = NULL;
  is_private = FALSE;

  G_LOCK (connection_lock);

  for (n = 0; n < n_construct_properties; n++)
    {
      if (g_strcmp0 (construct_properties[n].pspec->name, "bus-type") == 0)
        {
          GBusType bus_type;
          const gchar *starter_bus;

          bus_type = g_value_get_enum (construct_properties[n].value);
          switch (bus_type)
            {
            case G_BUS_TYPE_NONE:
              /* do nothing */
              break;

            case G_BUS_TYPE_SESSION:
              singleton = &the_session_bus;
              break;

            case G_BUS_TYPE_SYSTEM:
              singleton = &the_system_bus;
              break;

            case G_BUS_TYPE_STARTER:
              starter_bus = g_getenv ("DBUS_STARTER_BUS_TYPE");
              if (g_strcmp0 (starter_bus, "session") == 0)
                {
                  g_value_set_enum (construct_properties[n].value, G_BUS_TYPE_SESSION);
                  singleton = &the_session_bus;
                }
              else if (g_strcmp0 (starter_bus, "system") == 0)
                {
                  g_value_set_enum (construct_properties[n].value, G_BUS_TYPE_SYSTEM);
                  singleton = &the_system_bus;
                }
              else
                {
                  g_critical (_("Cannot construct a GDBusConnection object with bus_type G_BUS_TYPE_STARTER "
                                "because the DBUS_STARTER_BUS_TYPE environment variable is not set. "
                                "This is an error in the application or library using GDBus."));
                  goto out;
                }
              break;

            default:
              g_assert_not_reached ();
              break;
            }
        }
      else if (g_strcmp0 (construct_properties[n].pspec->name, "is-private") == 0)
        {
          is_private = g_value_get_boolean (construct_properties[n].value);
        }
    }

  if (is_private)
    singleton = NULL;

  if (singleton != NULL && *singleton != NULL)
    {
      object = g_object_ref (*singleton);
      goto out;
    }

  object = G_OBJECT_CLASS (g_dbus_connection_parent_class)->constructor (type,
                                                                         n_construct_properties,
                                                                         construct_properties);

  if (singleton != NULL)
    *singleton = G_DBUS_CONNECTION (object);

 out:
  G_UNLOCK (connection_lock);
  return object;
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
initable_init (GInitable       *initable,
               GCancellable    *cancellable,
               GError         **error)
{
  GDBusConnection *connection = G_DBUS_CONNECTION (initable);
  DBusConnection *dbus_1_connection;
  DBusError dbus_error;
  gboolean ret;

  G_LOCK (connection_lock);

  ret = FALSE;

  if (connection->priv->is_initialized)
    {
      if (connection->priv->dbus_1_connection != NULL)
        {
          ret = TRUE;
        }
      else
        {
          g_assert (connection->priv->initialization_error != NULL);
          g_propagate_error (error, g_error_copy (connection->priv->initialization_error));
        }
      goto out;
    }

  g_assert (connection->priv->dbus_1_connection == NULL);
  g_assert (connection->priv->initialization_error == NULL);

  dbus_error_init (&dbus_error);
  g_assert (connection->priv->bus_type != G_BUS_TYPE_NONE); // until we have constructors with @address
  if (connection->priv->is_private)
    {
      dbus_1_connection = dbus_bus_get_private (connection->priv->bus_type,
                                                &dbus_error);
    }
  else
    {
      dbus_1_connection = dbus_bus_get (connection->priv->bus_type,
                                        &dbus_error);
    }

  if (dbus_1_connection != NULL)
    {
      g_dbus_connection_set_dbus_1_connection (connection, dbus_1_connection);
      dbus_connection_unref (dbus_1_connection);
      ret = TRUE;
    }
  else
    {
      g_dbus_error_set_dbus_error (&connection->priv->initialization_error,
                                   &dbus_error,
                                   NULL,
                                   NULL);
      dbus_error_free (&dbus_error);
      g_propagate_error (error, g_error_copy (connection->priv->initialization_error));
    }

  connection->priv->is_initialized = TRUE;

 out:
  G_UNLOCK (connection_lock);
  return ret;
}

static void
initable_iface_init (GInitableIface *initable_iface)
{
  initable_iface->init = initable_init;
}

/* ---------------------------------------------------------------------------------------------------- */

static void
async_initable_init_async (GAsyncInitable     *initable,
                           gint                io_priority,
                           GCancellable       *cancellable,
                           GAsyncReadyCallback callback,
                           gpointer            user_data)
{
  GSimpleAsyncResult *simple;
  GError *error;

  simple = g_simple_async_result_new (G_OBJECT (initable),
                                      callback,
                                      user_data,
                                      async_initable_init_async);

  /* for now, we just do this asynchronously and complete in idle since libdbus has no way
   * to do it asynchronously
   */
  error = NULL;
  if (!initable_init (G_INITABLE (initable),
                      cancellable,
                      &error))
    {
      g_simple_async_result_set_from_error (simple, error);
      g_error_free (error);
    }

  g_simple_async_result_complete_in_idle (simple);
  g_object_unref (simple);
}

static gboolean
async_initable_init_finish (GAsyncInitable  *initable,
                            GAsyncResult    *res,
                            GError         **error)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (res);

  g_warn_if_fail (g_simple_async_result_get_source_tag (simple) == async_initable_init_async);

  if (g_simple_async_result_propagate_error (simple, error))
    return FALSE;
  return TRUE;
}

static void
async_initable_iface_init (GAsyncInitableIface *async_initable_iface)
{
  async_initable_iface->init_async = async_initable_init_async;
  async_initable_iface->init_finish = async_initable_init_finish;
}

/* ---------------------------------------------------------------------------------------------------- */

/**
 * g_dbus_connection_bus_get_sync:
 * @bus_type: A #GBusType.
 * @cancellable: A #GCancellable or %NULL.
 * @error: Return location for error or %NULL.
 *
 * Synchronously connects to the message bus specified by @bus_type.
 * Note that the returned object may shared with other callers,
 * e.g. if two separate parts of a process calls this function with
 * the same @bus_type, they will share the same object.
 *
 * Use g_dbus_connection_bus_get_private_sync() to get a private
 * connection.
 *
 * This is a synchronous failable constructor. See
 * g_dbus_connection_bus_get() and g_dbus_connection_bus_get_finish()
 * for the asynchronous version.
 *
 * Returns: A #GDBusConnection or %NULL if @error is set. Free with g_object_unref().
 **/
GDBusConnection *
g_dbus_connection_bus_get_sync (GBusType            bus_type,
                                GCancellable       *cancellable,
                                GError            **error)
{
  GInitable *initable;

  initable = g_initable_new (G_TYPE_DBUS_CONNECTION,
                             cancellable,
                             error,
                             "bus-type", bus_type,
                             NULL);

  if (initable != NULL)
    return G_DBUS_CONNECTION (initable);
  else
    return NULL;
}

/**
 * g_dbus_connection_bus_get:
 * @bus_type: A #GBusType.
 * @cancellable: A #GCancellable or %NULL.
 * @callback: A #GAsyncReadyCallback to call when the request is satisfied
 * @user_data: The data to pass to @callback.
 *
 * Asynchronously connects to the message bus specified by @bus_type.
 *
 * When the operation is finished, @callback will be invoked. You can
 * then call g_dbus_connection_bus_get_finish() to get the result of
 * the operation.
 *
 * Use g_dbus_connection_bus_get_private() to get a private
 * connection.
 *
 * This is a asynchronous failable constructor. See
 * g_dbus_connection_bus_get_sync() for the synchronous version.
 **/
void
g_dbus_connection_bus_get (GBusType             bus_type,
                           GCancellable        *cancellable,
                           GAsyncReadyCallback  callback,
                           gpointer             user_data)
{
  g_async_initable_new_async (G_TYPE_DBUS_CONNECTION,
                              G_PRIORITY_DEFAULT,
                              cancellable,
                              callback,
                              user_data,
                              "bus-type", bus_type,
                              NULL);
}

/**
 * g_dbus_connection_bus_get_finish:
 * @res: A #GAsyncResult obtained from the #GAsyncReadyCallback passed to g_dbus_connection_bus_get().
 * @error: Return location for error or %NULL.
 *
 * Finishes an operation started with g_dbus_connection_bus_get().
 *
 * Note that the returned object may shared with other callers,
 * e.g. if two separate parts of a process calls this function with
 * the same @bus_type, they will share the same object.
 *
 * Returns: A #GDBusConnection or %NULL if @error is set. Free with g_object_unref().
 **/
GDBusConnection *
g_dbus_connection_bus_get_finish (GAsyncResult  *res,
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
    return G_DBUS_CONNECTION (object);
  else
    return NULL;
}


/* ---------------------------------------------------------------------------------------------------- */

/**
 * g_dbus_connection_bus_get_private_sync:
 * @bus_type: A #GBusType.
 * @cancellable: A #GCancellable or %NULL.
 * @error: Return location for error or %NULL.
 *
 * Like g_dbus_connection_bus_get_sync() but gets a connection that is not
 * shared with other callers.
 *
 * Returns: A #GDBusConnection. Free with g_object_unref().
 **/
GDBusConnection *
g_dbus_connection_bus_get_private_sync (GBusType        bus_type,
                                        GCancellable   *cancellable,
                                        GError        **error)
{
  GInitable *initable;

  initable = g_initable_new (G_TYPE_DBUS_CONNECTION,
                             cancellable,
                             error,
                             "bus-type", bus_type,
                             "is-private", TRUE,
                             NULL);

  if (initable != NULL)
    return G_DBUS_CONNECTION (initable);
  else
    return NULL;
}

/**
 * g_dbus_connection_bus_get_private:
 * @bus_type: A #GBusType.
 * @cancellable: A #GCancellable or %NULL.
 * @callback: A #GAsyncReadyCallback to call when the request is satisfied
 * @user_data: The data to pass to @callback.
 *
 * Asynchronously connects to the message bus specified by @bus_type
 * using a private connection.
 *
 * When the operation is finished, @callback will be invoked. You can
 * then call g_dbus_connection_bus_get_finish() to get the result of
 * the operation.
 *
 * Use g_dbus_connection_bus_get() to get a shared connection.
 *
 * This is a asynchronous failable constructor. See
 * g_dbus_connection_bus_get_private_sync() for the synchronous
 * version.
 **/
void
g_dbus_connection_bus_get_private (GBusType             bus_type,
                                   GCancellable        *cancellable,
                                   GAsyncReadyCallback  callback,
                                   gpointer             user_data)
{
  g_async_initable_new_async (G_TYPE_DBUS_CONNECTION,
                              G_PRIORITY_DEFAULT,
                              cancellable,
                              callback,
                              user_data,
                              "bus-type", bus_type,
                              "is-private", TRUE,
                              NULL);
}

/**
 * g_dbus_connection_bus_get_private_finish:
 * @res: A #GAsyncResult obtained from the #GAsyncReadyCallback passed to g_dbus_connection_bus_get_private().
 * @error: Return location for error or %NULL.
 *
 * Finishes an operation started with g_dbus_connection_bus_get_private().
 *
 * The returned object is never shared with other callers.
 *
 * Returns: A #GDBusConnection or %NULL if @error is set. Free with g_object_unref().
 **/
GDBusConnection *
g_dbus_connection_bus_get_private_finish (GAsyncResult  *res,
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
    return G_DBUS_CONNECTION (object);
  else
    return NULL;
}

/* ---------------------------------------------------------------------------------------------------- */

/**
 * g_dbus_connection_set_exit_on_disconnect:
 * @connection: A #GDBusConnection.
 * @exit_on_disconnect: Whether _exit() should be called when @connection is
 * disconnected.
 *
 * Sets whether _exit() should be called when @connection is disconnected.
 **/
void
g_dbus_connection_set_exit_on_disconnect (GDBusConnection *connection,
                                          gboolean         exit_on_disconnect)
{
  g_return_if_fail (G_IS_DBUS_CONNECTION (connection));

  connection->priv->exit_on_disconnect = exit_on_disconnect;
  if (connection->priv->dbus_1_connection != NULL)
    dbus_connection_set_exit_on_disconnect (connection->priv->dbus_1_connection,
                                            connection->priv->exit_on_disconnect);
}

/**
 * g_dbus_connection_get_unique_name:
 * @connection: A #GDBusConnection.
 *
 * Gets the unique name of @connection as assigned by the message bus.
 *
 * Returns: The unique name or %NULL if the connection is disconnected
 * or @connection is not a message bus connection. Do not free this
 * string, it is owned by @connection.
 **/
const gchar *
g_dbus_connection_get_unique_name (GDBusConnection *connection)
{
  g_return_val_if_fail (G_IS_DBUS_CONNECTION (connection), NULL);

  if (connection->priv->bus_type == G_BUS_TYPE_NONE)
    return NULL;

  if (connection->priv->dbus_1_connection != NULL)
    return dbus_bus_get_unique_name (connection->priv->dbus_1_connection);
  else
    return NULL;
}

/* ---------------------------------------------------------------------------------------------------- */

/**
 * g_dbus_connection_send_dbus_1_message:
 * @connection: A #GDBusConnection.
 * @message: A #DBusMessage
 *
 * <para><note>
 * This function is marked as unstable API. You must include <literal>gdbus/gdbus-lowlevel.h</literal> to use it.
 * </note></para>
 *
 * Sends @message on @connection. If @connection is disconnected, this function is a no-op.
 *
 * This function is intended for use by object mappings only.
 **/
void
g_dbus_connection_send_dbus_1_message (GDBusConnection    *connection,
                                       DBusMessage        *message)
{
  g_return_if_fail (G_IS_DBUS_CONNECTION (connection));
  g_return_if_fail (message != NULL);

  if (connection->priv->dbus_1_connection == NULL)
    goto out;

  if (!dbus_connection_send (connection->priv->dbus_1_connection,
                             message,
                             NULL))
    _g_dbus_oom ();

 out:
  ;
}

/* ---------------------------------------------------------------------------------------------------- */

static void
send_dbus_1_message_with_reply_cb (DBusPendingCall *pending_call,
                                   void            *user_data)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (user_data);
  GDBusConnection *connection;
  GCancellable *cancellable;
  gulong cancellable_handler_id;
  DBusMessage *reply;

  G_LOCK (connection_lock);
  cancellable_handler_id = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (simple), "cancellable-handler-id"));
  connection = G_DBUS_CONNECTION (g_async_result_get_source_object (G_ASYNC_RESULT (simple)));
  G_UNLOCK (connection_lock);

  cancellable = g_object_get_data (G_OBJECT (simple), "cancellable");

  if (cancellable_handler_id > 0)
    g_cancellable_disconnect (cancellable, cancellable_handler_id);

  if (pending_call == NULL)
    {
      g_simple_async_result_set_error (simple,
                                       G_DBUS_ERROR,
                                       G_DBUS_ERROR_CANCELLED,
                                       _("Operation was cancelled"));
    }
  else
    {
      reply = dbus_pending_call_steal_reply (pending_call);
      g_assert (reply != NULL);
      g_simple_async_result_set_op_res_gpointer (simple, reply, (GDestroyNotify) dbus_message_unref);
    }
  g_simple_async_result_complete_in_idle (simple);
  g_object_unref (connection);
  g_object_unref (simple);
}

static gboolean
send_dbus_1_message_with_reply_cancelled_in_idle (gpointer user_data)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (user_data);
  send_dbus_1_message_with_reply_cb (NULL, simple);
  return FALSE;
}

static void
send_dbus_1_message_with_reply_cancelled_cb (GCancellable *cancellable,
                                             gpointer      user_data)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (user_data);
  DBusPendingCall *pending_call;

  pending_call = g_object_get_data (G_OBJECT (simple), "dbus-1-pending-call");
  dbus_pending_call_cancel (pending_call);

  g_idle_add (send_dbus_1_message_with_reply_cancelled_in_idle, simple);
}

/**
 * g_dbus_connection_send_dbus_1_message_with_reply:
 * @connection: A #GDBusConnection.
 * @message: A #DBusMessage
 * @timeout_msec: The timeout in milliseconds or -1 to use the default timeout.
 * @cancellable: A #GCancellable or %NULL.
 * @callback: Callback function to invoke when the reply is ready.
 * @user_data: User data to pass to @callback.
 *
 * <para><note>
 * This function is marked as unstable API. You must include <literal>gdbus/gdbus-lowlevel.h</literal> to use it.
 * </note></para>
 *
 * Asynchronously sends @message on @connection and invokes @callback when the reply is
 * ready. In @callback you should call g_dbus_connection_send_dbus_1_message_with_reply_finish() to
 * get the reply.
 *
 * Note that it is considered a programming error if @message is not a
 * method-call message.
 *
 * If @connection is disconnected then the operatoin will fail with %G_DBUS_ERROR_DISCONNECTED.
 *
 * This function is intended for use by object mappings only.
 *
 **/
void
g_dbus_connection_send_dbus_1_message_with_reply (GDBusConnection    *connection,
                                                  DBusMessage        *message,
                                                  gint                timeout_msec,
                                                  GCancellable       *cancellable,
                                                  GAsyncReadyCallback callback,
                                                  gpointer            user_data)
{
  GSimpleAsyncResult *simple;
  DBusPendingCall *pending_call;
  gulong cancellable_handler_id;

  g_return_if_fail (G_IS_DBUS_CONNECTION (connection));
  g_return_if_fail (callback != NULL);
  g_return_if_fail (message != NULL);
  g_return_if_fail (dbus_message_get_type (message) == DBUS_MESSAGE_TYPE_METHOD_CALL);

  G_LOCK (connection_lock);

  simple = g_simple_async_result_new (G_OBJECT (connection),
                                      callback,
                                      user_data,
                                      g_dbus_connection_send_dbus_1_message_with_reply);

  /* don't even send a message if already cancelled */
  if (g_cancellable_is_cancelled (cancellable))
    {
      g_simple_async_result_set_error (simple,
                                       G_DBUS_ERROR,
                                       G_DBUS_ERROR_CANCELLED,
                                       _("Operation was cancelled"));
      g_simple_async_result_complete_in_idle (simple);
      g_object_unref (simple);
      goto out;
    }

  if (connection->priv->dbus_1_connection == NULL)
    {
      g_simple_async_result_set_error (simple,
                                       G_DBUS_ERROR,
                                       G_DBUS_ERROR_DISCONNECTED,
                                       _("Not connected"));
      g_simple_async_result_complete_in_idle (simple);
      g_object_unref (simple);
      goto out;
    }

  if (!dbus_connection_send_with_reply (connection->priv->dbus_1_connection,
                                        message,
                                        &pending_call,
                                        timeout_msec))
    _g_dbus_oom ();

  if (pending_call == NULL)
    {
      g_simple_async_result_set_error (simple,
                                       G_DBUS_ERROR,
                                       G_DBUS_ERROR_FAILED,
                                       _("Not connected"));
      g_simple_async_result_complete_in_idle (simple);
      g_object_unref (simple);
      goto out;
    }

  g_object_set_data_full (G_OBJECT (simple),
                          "dbus-1-pending-call",
                          pending_call,
                          (GDestroyNotify) dbus_pending_call_unref);

  g_object_set_data (G_OBJECT (simple),
                     "cancellable",
                     cancellable);

  dbus_pending_call_set_notify (pending_call,
                                send_dbus_1_message_with_reply_cb,
                                simple,
                                NULL);

  cancellable_handler_id = 0;
  if (cancellable != NULL)
    {
      /* use the lock to ensure cancellable-handler-id is set on simple before trying to get it
       * in send_dbus_1_message_with_reply_cb()
       */
      cancellable_handler_id = g_cancellable_connect (cancellable,
                                                      G_CALLBACK (send_dbus_1_message_with_reply_cancelled_cb),
                                                      simple,
                                                      NULL);
      g_object_set_data (G_OBJECT (simple),
                         "cancellable-handler-id",
                         GUINT_TO_POINTER (cancellable_handler_id));
    }

 out:
  G_UNLOCK (connection_lock);
}

/**
 * g_dbus_connection_send_dbus_1_message_with_reply_finish:
 * @connection: A #GDBusConnection.
 * @res: A #GAsyncResult obtained from the #GAsyncReadyCallback function passed
 * to g_dbus_connection_send_dbus_1_message_with_reply().
 * @error: Return location for error or %NULL.
 *
 * <para><note>
 * This function is marked as unstable API. You must include <literal>gdbus/gdbus-lowlevel.h</literal> to use it.
 * </note></para>
 *
 * Finishes sending a message with reply.
 *
 * Note that @error is only set if the #GCancellable passed to g_dbus_connection_send_dbus_1_message_with_reply()
 * was cancelled (in which case %G_DBUS_ERROR_CANCELLED is returned) or if @connection was disconnected (in
 * which case %G_DBUS_ERROR_DISCONNECTED is returned). Specifically, the returned #DBusMessage message can
 * be an error message (cf. dbus_message_is_error()). If your object mapping uses #GError you can use
 * the utility function g_dbus_error_new_for_dbus_error() to map this to a #GError.
 *
 * This function is intended for use by object mappings only.
 *
 * Returns: A #DBusMessage or %NULL if @error is set. Free with dbus_message_unref().
 **/
DBusMessage *
g_dbus_connection_send_dbus_1_message_with_reply_finish (GDBusConnection   *connection,
                                                         GAsyncResult      *res,
                                                         GError           **error)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (res);
  DBusMessage *reply;

  g_warn_if_fail (g_simple_async_result_get_source_tag (simple) == g_dbus_connection_send_dbus_1_message_with_reply);

  reply = NULL;
  if (g_simple_async_result_propagate_error (simple, error))
    goto out;

  reply = dbus_message_ref (g_simple_async_result_get_op_res_gpointer (simple));

 out:
  return reply;
}

/* ---------------------------------------------------------------------------------------------------- */

static void
send_dbus_1_message_with_reply_sync_cancelled_cb (GCancellable *cancellable,
                                                  gpointer      user_data)
{
  DBusPendingCall *pending_call = user_data;

  dbus_pending_call_cancel (pending_call);
}

/**
 * g_dbus_connection_send_dbus_1_message_with_reply_sync:
 * @connection: A #GDBusConnection.
 * @message: A #DBusMessage.
 * @timeout_msec: The timeout in milliseconds or -1 to use the default timeout.
 * @cancellable: A #GCancellable or %NULL.
 * @error: Return location for error or %NULL.
 *
 * <para><note>
 * This function is marked as unstable API. You must include <literal>gdbus/gdbus-lowlevel.h</literal> to use it.
 * </note></para>
 *
 * Synchronously sends @message on @connection and blocks the calling
 * thread until a reply is ready. TODO: some notes about threading and
 * blocking the mainloop etc.
 *
 * Returns: A #DBusMessage or %NULL if @error is set. Free with dbus_message_unref().
 **/
DBusMessage *
g_dbus_connection_send_dbus_1_message_with_reply_sync (GDBusConnection    *connection,
                                                       DBusMessage        *message,
                                                       gint                timeout_msec,
                                                       GCancellable       *cancellable,
                                                       GError            **error)
{
  gulong cancellable_handler_id;
  DBusMessage *result;
  DBusPendingCall *pending_call;

  g_return_val_if_fail (G_IS_DBUS_CONNECTION (connection), 0);
  g_return_val_if_fail (message != NULL, 0);
  g_return_val_if_fail (dbus_message_get_type (message) == DBUS_MESSAGE_TYPE_METHOD_CALL, 0);

  result = NULL;

  G_LOCK (connection_lock);

  /* don't even send a message if already cancelled */
  if (g_cancellable_is_cancelled (cancellable))
    {
      g_set_error (error,
                   G_DBUS_ERROR,
                   G_DBUS_ERROR_CANCELLED,
                   _("Operation was cancelled"));
      G_UNLOCK (connection_lock);
      goto out;
    }

  if (connection->priv->dbus_1_connection == NULL)
    {
      g_set_error (error,
                   G_DBUS_ERROR,
                   G_DBUS_ERROR_FAILED,
                   _("Not connected"));
      G_UNLOCK (connection_lock);
      goto out;
    }

  if (!dbus_connection_send_with_reply (connection->priv->dbus_1_connection,
                                        message,
                                        &pending_call,
                                        timeout_msec))
    _g_dbus_oom ();

  if (pending_call == NULL)
    {
      g_set_error (error,
                   G_DBUS_ERROR,
                   G_DBUS_ERROR_FAILED,
                   _("Not connected"));
      G_UNLOCK (connection_lock);
      goto out;
    }

  cancellable_handler_id = 0;
  if (cancellable != NULL)
    {
      /* use the lock to ensure cancellable-handler-id is set on simple before trying to get it
       * in send_dbus_1_message_with_reply_cb()
       */
      cancellable_handler_id = g_cancellable_connect (cancellable,
                                                      G_CALLBACK (send_dbus_1_message_with_reply_sync_cancelled_cb),
                                                      pending_call,
                                                      NULL);
    }

  G_UNLOCK (connection_lock);

  /* block without holding the lock */
  dbus_pending_call_block (pending_call);

  if (cancellable_handler_id > 0)
    {
      g_cancellable_disconnect (cancellable,
                                cancellable_handler_id);
    }

  result = dbus_pending_call_steal_reply (pending_call);
  if (pending_call == NULL)
    {
      g_set_error (error,
                   G_DBUS_ERROR,
                   G_DBUS_ERROR_CANCELLED,
                   _("Operation was cancelled"));
    }

  dbus_pending_call_unref (pending_call);

 out:
  return result;
}

/* ---------------------------------------------------------------------------------------------------- */

typedef struct
{
  gchar *rule;
  gchar *sender;
  gchar *interface_name;
  gchar *member;
  gchar *object_path;
  gchar *arg0;
  GArray *subscribers;
} SignalData;

typedef struct
{
  GDBusSignalCallback1 callback;
  gpointer user_data;
  GDestroyNotify user_data_free_func;
  guint id;
} SignalSubscriber;

static void
signal_data_free (SignalData *data)
{
  g_free (data->rule);
  g_free (data->sender);
  g_free (data->interface_name);
  g_free (data->member);
  g_free (data->object_path);
  g_free (data->arg0);
  g_array_free (data->subscribers, TRUE);
  g_free (data);
}

static gchar *
args_to_rule (const gchar         *sender,
              const gchar         *interface_name,
              const gchar         *member,
              const gchar         *object_path,
              const gchar         *arg0)
{
  GString *rule;

  rule = g_string_new ("type='signal'");
  if (sender != NULL)
    g_string_append_printf (rule, ",sender='%s'", sender);
  if (interface_name != NULL)
    g_string_append_printf (rule, ",interface='%s'", interface_name);
  if (member != NULL)
    g_string_append_printf (rule, ",member='%s'", member);
  if (object_path != NULL)
    g_string_append_printf (rule, ",path='%s'", object_path);
  if (arg0 != NULL)
    g_string_append_printf (rule, ",arg0='%s'", arg0);

  return g_string_free (rule, FALSE);
}

static guint _global_subscriber_id = 1;

/* ---------------------------------------------------------------------------------------------------- */

static void
add_match_cb (DBusPendingCall *pending_call,
              void            *user_data)
{
  DBusMessage *reply;
  DBusError dbus_error;

  reply = dbus_pending_call_steal_reply (pending_call);
  g_assert (reply != NULL);

  dbus_error_init (&dbus_error);
  if (dbus_set_error_from_message (&dbus_error, reply))
    {
      if (g_strcmp0 (dbus_error.name, "org.freedesktop.DBus.Error.OOM") == 0)
        {
          g_critical ("Message bus reported OOM when trying to add match rule: %s: %s",
                      dbus_error.name,
                      dbus_error.message);
          _g_dbus_oom ();
        }

      /* Don't report other errors; the bus might have gone away while sending the message
       * so @dbus_error might be a locally generated error.
       */

      dbus_error_free (&dbus_error);
    }
}

static void
add_match_rule (GDBusConnection *connection,
                const gchar     *match_rule)
{
  DBusMessage *message;
  DBusPendingCall *pending_call;

  if ((message = dbus_message_new_method_call (DBUS_SERVICE_DBUS,
                                               DBUS_PATH_DBUS,
                                               DBUS_INTERFACE_DBUS,
                                               "AddMatch")) == NULL)
    _g_dbus_oom ();
  if (!dbus_message_append_args (message,
                                 DBUS_TYPE_STRING, &match_rule,
                                 DBUS_TYPE_INVALID))
    _g_dbus_oom ();

  /* don't use g_dbus_connection_send_dbus_1_message_with_reply() since we don't want to ref @connection */
  if (!dbus_connection_send_with_reply (connection->priv->dbus_1_connection,
                                        message,
                                        &pending_call,
                                        -1))
    _g_dbus_oom ();

  dbus_pending_call_set_notify (pending_call,
                                add_match_cb,
                                NULL,
                                NULL);

  dbus_message_unref (message);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
remove_match_cb (DBusPendingCall *pending_call,
                 void            *user_data)
{
  DBusMessage *reply;
  DBusError dbus_error;

  reply = dbus_pending_call_steal_reply (pending_call);
  g_assert (reply != NULL);

  dbus_error_init (&dbus_error);
  if (dbus_set_error_from_message (&dbus_error, reply))
    {
      if (g_strcmp0 (dbus_error.name, "org.freedesktop.DBus.Error.MatchRuleNotFound") == 0)
        {
          g_warning ("Message bus reported error removing match rule: %s: %s\n"
                     "This is a bug in GDBus.",
                     dbus_error.name,
                     dbus_error.message);
        }
      dbus_error_free (&dbus_error);
    }
}

static void
remove_match_rule (GDBusConnection *connection,
                   const gchar     *match_rule)
{
  DBusMessage *message;
  DBusPendingCall *pending_call;

  if ((message = dbus_message_new_method_call (DBUS_SERVICE_DBUS,
                                               DBUS_PATH_DBUS,
                                               DBUS_INTERFACE_DBUS,
                                               "RemoveMatch")) == NULL)
    _g_dbus_oom ();
  if (!dbus_message_append_args (message,
                                 DBUS_TYPE_STRING, &match_rule,
                                 DBUS_TYPE_INVALID))
    _g_dbus_oom ();

  /* don't use g_dbus_connection_send_dbus_1_message_with_reply() since we don't want to ref @connection */
  if (!dbus_connection_send_with_reply (connection->priv->dbus_1_connection,
                                        message,
                                        &pending_call,
                                        -1))
    _g_dbus_oom ();

  dbus_pending_call_set_notify (pending_call,
                                remove_match_cb,
                                NULL,
                                NULL);

  dbus_message_unref (message);
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
is_signal_data_for_name_lost_or_acquired (SignalData *signal_data)
{
  return g_strcmp0 (signal_data->sender, DBUS_SERVICE_DBUS) == 0 &&
    g_strcmp0 (signal_data->interface_name, DBUS_INTERFACE_DBUS) == 0 &&
    g_strcmp0 (signal_data->object_path, DBUS_PATH_DBUS) == 0 &&
    (g_strcmp0 (signal_data->member, "NameLost") == 0 ||
     g_strcmp0 (signal_data->member, "NameAcquired") == 0);
}

/* ---------------------------------------------------------------------------------------------------- */

/**
 * g_dbus_connection_dbus_1_signal_subscribe:
 * @connection: A #GDBusConnection.
 * @sender: Sender name to match on. Must be either <literal>org.freedesktop.DBus</literal> (for listening to signals from the message bus daemon) or a unique name or %NULL to listen from all senders.
 * @interface_name: D-Bus interface name to match on or %NULL to match on all interfaces.
 * @member: D-Bus signal name to match on or %NULL to match on all signals.
 * @object_path: Object path to match on or %NULL to match on all object paths.
 * @arg0: Contents of first string argument to match on or %NULL to match on all kinds of arguments.
 * @callback: Callback to invoke when there is a signal matching the requested data.
 * @user_data: User data to pass to @callback.
 * @user_data_free_func: Function to free @user_data when subscription is removed or %NULL.
 *
 * <para><note>
 * This function is marked as unstable API. You must include <literal>gdbus/gdbus-lowlevel.h</literal> to use it.
 * </note></para>
 *
 * Subscribes to signals on @connection and invokes @callback with a #DBusMessage whenever the signal
 * is received.
 *
 * It is considered a programming error to use this function if @connection has been disconnected.
 *
 * Note that if @sender is not <literal>org.freedesktop.DBus</literal> (for listening to signals from the
 * message bus daemon), then it needs to be a unique bus name or %NULL (for listening to signals from any
 * name) - you cannot pass a name like <literal>com.example.MyApp</literal>.
 * Use e.g. g_bus_watch_name() to find the unique name for the owner of the name you are interested in. Also note
 * that this function does not remove a subscription if @sender vanishes from the bus. You have to manually
 * call g_dbus_connection_dbus_1_signal_unsubscribe() to remove a subscription.
 *
 * Returns: A subscription identifier that can be used with g_dbus_connection_dbus_1_signal_unsubscribe().
 **/
guint
g_dbus_connection_dbus_1_signal_subscribe (GDBusConnection     *connection,
                                           const gchar         *sender,
                                           const gchar         *interface_name,
                                           const gchar         *member,
                                           const gchar         *object_path,
                                           const gchar         *arg0,
                                           GDBusSignalCallback1 callback,
                                           gpointer             user_data,
                                           GDestroyNotify       user_data_free_func)
{
  gchar *rule;
  SignalData *signal_data;
  SignalSubscriber subscriber;
  GPtrArray *signal_data_array;

  /* Right now we abort if AddMatch() fails since it can only fail with the bus being in
   * an OOM condition. We might want to change that but that would involve making
   * g_dbus_connection_dbus_1_signal_subscribe() asynchronous and having the call sites
   * handle that. And there's really no sensible way of handling this short of retrying
   * to add the match rule... and then there's the little thing that, hey, maybe there's
   * a reason the bus in an OOM condition.
   *
   * Doable, but not really sure it's worth it...
   *
   * TODO XXX MERGE_BLOCKER
   */

  g_return_val_if_fail (G_IS_DBUS_CONNECTION (connection), 0);
  g_return_val_if_fail (!g_dbus_connection_get_is_disconnected (connection), 0);
  g_return_val_if_fail (sender == NULL || ((strcmp (sender, DBUS_SERVICE_DBUS) == 0 || sender[0] == ':')), 0);
  g_return_val_if_fail (callback != NULL, 0);
  /* TODO: check that passed in data is well-formed */

  G_LOCK (connection_lock);

  rule = args_to_rule (sender, interface_name, member, object_path, arg0);

  if (sender == NULL)
    sender = "";

  subscriber.callback = callback;
  subscriber.user_data = user_data;
  subscriber.user_data_free_func = user_data_free_func;
  subscriber.id = _global_subscriber_id++; /* TODO: overflow etc. */

  /* see if we've already have this rule */
  signal_data = g_hash_table_lookup (connection->priv->map_rule_to_signal_data, rule);
  if (signal_data != NULL)
    {
      g_array_append_val (signal_data->subscribers, subscriber);
      g_free (rule);
      goto out;
    }

  signal_data = g_new0 (SignalData, 1);
  signal_data->rule           = rule;
  signal_data->sender         = g_strdup (sender);
  signal_data->interface_name = g_strdup (interface_name);
  signal_data->member         = g_strdup (member);
  signal_data->object_path    = g_strdup (object_path);
  signal_data->arg0           = g_strdup (arg0);
  signal_data->subscribers    = g_array_new (FALSE, FALSE, sizeof (SignalSubscriber));
  g_array_append_val (signal_data->subscribers, subscriber);

  g_hash_table_insert (connection->priv->map_rule_to_signal_data,
                       signal_data->rule,
                       signal_data);

  /* Add the match rule to the bus...
   *
   * Avoid adding match rules for NameLost and NameAcquired messages - the bus will
   * always send such messages to to us.
   */
  if (!is_signal_data_for_name_lost_or_acquired (signal_data))
    add_match_rule (connection, signal_data->rule);

 out:
  g_hash_table_insert (connection->priv->map_id_to_signal_data,
                       GUINT_TO_POINTER (subscriber.id),
                       signal_data);

  signal_data_array = g_hash_table_lookup (connection->priv->map_sender_to_signal_data_array,
                                           signal_data->sender);
  if (signal_data_array == NULL)
    {
      signal_data_array = g_ptr_array_new ();
      g_hash_table_insert (connection->priv->map_sender_to_signal_data_array,
                           g_strdup (signal_data->sender),
                           signal_data_array);
    }
  g_ptr_array_add (signal_data_array, signal_data);

  G_UNLOCK (connection_lock);

  return subscriber.id;
}

/* ---------------------------------------------------------------------------------------------------- */

/* must hold lock when calling this */
static void
unsubscribe_id_internal (GDBusConnection    *connection,
                         guint               subscription_id,
                         GArray             *out_removed_subscribers)
{
  SignalData *signal_data;
  GPtrArray *signal_data_array;
  guint n;

  signal_data = g_hash_table_lookup (connection->priv->map_id_to_signal_data,
                                     GUINT_TO_POINTER (subscription_id));
  if (signal_data == NULL)
    {
      /* Don't warn here, we may have thrown all subscriptions out when the connection was closed */
      goto out;
    }

  for (n = 0; n < signal_data->subscribers->len; n++)
    {
      SignalSubscriber *subscriber;

      subscriber = &(g_array_index (signal_data->subscribers, SignalSubscriber, n));
      if (subscriber->id != subscription_id)
        continue;

      g_assert (g_hash_table_remove (connection->priv->map_id_to_signal_data,
                                     GUINT_TO_POINTER (subscription_id)));
      g_array_append_val (out_removed_subscribers, *subscriber);
      g_array_remove_index (signal_data->subscribers, n);

      if (signal_data->subscribers->len == 0)
        g_assert (g_hash_table_remove (connection->priv->map_rule_to_signal_data, signal_data->rule));

      signal_data_array = g_hash_table_lookup (connection->priv->map_sender_to_signal_data_array,
                                               signal_data->sender);
      g_assert (signal_data_array != NULL);
      g_assert (g_ptr_array_remove (signal_data_array, signal_data));

      if (signal_data_array->len == 0)
        {
          g_assert (g_hash_table_remove (connection->priv->map_sender_to_signal_data_array, signal_data->sender));

          /* remove the match rule from the bus unless NameLost or NameAcquired (see subscribe()) */
          if (!is_signal_data_for_name_lost_or_acquired (signal_data) &&
              connection->priv->dbus_1_connection != NULL)
            remove_match_rule (connection, signal_data->rule);

          signal_data_free (signal_data);
        }

      goto out;
    }

  g_assert_not_reached ();

 out:
  ;
}

/**
 * g_dbus_connection_dbus_1_signal_unsubscribe:
 * @connection: A #GDBusConnection.
 * @subscription_id: A subscription id obtained from g_dbus_connection_dbus_1_signal_subscribe().
 *
 * <para><note>
 * This function is marked as unstable API. You must include <literal>gdbus/gdbus-lowlevel.h</literal> to use it.
 * </note></para>
 *
 * Unsubscribes from signals.
 **/
void
g_dbus_connection_dbus_1_signal_unsubscribe (GDBusConnection    *connection,
                                             guint               subscription_id)
{
  GArray *subscribers;
  guint n;

  subscribers = g_array_new (FALSE, FALSE, sizeof (SignalSubscriber));

  G_LOCK (connection_lock);
  unsubscribe_id_internal (connection,
                           subscription_id,
                           subscribers);
  G_UNLOCK (connection_lock);

  /* invariant */
  g_assert (subscribers->len == 0 || subscribers->len == 1);

  /* call GDestroyNotify without lock held */
  for (n = 0; n < subscribers->len; n++)
    {
      SignalSubscriber *subscriber;
      subscriber = &(g_array_index (subscribers, SignalSubscriber, n));
      if (subscriber->user_data_free_func != NULL)
        subscriber->user_data_free_func (subscriber->user_data);
    }

  g_array_free (subscribers, TRUE);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
add_callbacks (GPtrArray   *signal_data_array,
               GArray      *callbacks,
               DBusMessage *message)
{
  guint n, m;

  /* TODO: if this is slow, then we can change signal_data_array into
   *       map_object_path_to_signal_data_array or something.
   */
  for (n = 0; n < signal_data_array->len; n++)
    {
      SignalData *signal_data = signal_data_array->pdata[n];
      const gchar *arg0;

      if (signal_data->interface_name != NULL &&
          g_strcmp0 (signal_data->interface_name, dbus_message_get_interface (message)) != 0)
        continue;

      if (signal_data->member != NULL &&
          g_strcmp0 (signal_data->member, dbus_message_get_member (message)) != 0)
        continue;

      if (signal_data->object_path != NULL &&
          g_strcmp0 (signal_data->object_path, dbus_message_get_path (message)) != 0)
        continue;

      if (signal_data->arg0 != NULL)
        {
          if (!dbus_message_get_args (message,
                                      NULL,
                                      DBUS_TYPE_STRING, &arg0,
                                      DBUS_TYPE_INVALID))
            continue;

          if (g_strcmp0 (signal_data->arg0, arg0) != 0)
            continue;
        }

      for (m = 0; m < signal_data->subscribers->len; m++)
        {
          SignalSubscriber *subscriber;
          subscriber = &(g_array_index (signal_data->subscribers, SignalSubscriber, m));

          g_array_append_val (callbacks, *subscriber);
        }
    }
}

/* do not call with any locks held */
static void
distribute_signals (GDBusConnection *connection,
                    DBusMessage     *message)
{
  const gchar *sender;
  GPtrArray *signal_data_array;
  GArray *callbacks;
  guint n;

  sender = dbus_message_get_sender (message);
  if (sender == NULL)
    goto out;

  G_LOCK (connection_lock);

  /* collect callbacks with lock held, invoke them without holding lock */
  callbacks = g_array_new (FALSE, FALSE, sizeof (SignalSubscriber));

  /* collect subcsribers that match on sender */
  signal_data_array = g_hash_table_lookup (connection->priv->map_sender_to_signal_data_array, sender);
  if (signal_data_array != NULL) {
    add_callbacks (signal_data_array, callbacks, message);
  }

  /* collect subcsribers not matching on sender */
  signal_data_array = g_hash_table_lookup (connection->priv->map_sender_to_signal_data_array, "");
  if (signal_data_array != NULL) {
    add_callbacks (signal_data_array, callbacks, message);
  }

  G_UNLOCK (connection_lock);

  for (n = 0; n < callbacks->len; n++)
    {
      SignalSubscriber *subscriber;

      subscriber = &(g_array_index (callbacks, SignalSubscriber, n));

      subscriber->callback (connection,
                            message,
                            subscriber->user_data);
    }

  g_array_free (callbacks, TRUE);

out:
  ;
}

/* ---------------------------------------------------------------------------------------------------- */

/* called from finalize(), removes all subscriptions */
static void
purge_all_signal_subscriptions (GDBusConnection *connection)
{
  GHashTableIter iter;
  gpointer key;
  GArray *ids;
  GArray *subscribers;
  guint n;

  G_LOCK (connection_lock);
  ids = g_array_new (FALSE, FALSE, sizeof (guint));
  g_hash_table_iter_init (&iter, connection->priv->map_id_to_signal_data);
  while (g_hash_table_iter_next (&iter, &key, NULL))
    {
      guint subscription_id = GPOINTER_TO_UINT (key);
      g_array_append_val (ids, subscription_id);
    }

  subscribers = g_array_new (FALSE, FALSE, sizeof (SignalSubscriber));
  for (n = 0; n < ids->len; n++)
    {
      guint subscription_id = g_array_index (ids, guint, n);
      unsubscribe_id_internal (connection,
                               subscription_id,
                               subscribers);
    }
  g_array_free (ids, TRUE);

  G_UNLOCK (connection_lock);

  /* call GDestroyNotify without lock held */
  for (n = 0; n < subscribers->len; n++)
    {
      SignalSubscriber *subscriber;
      subscriber = &(g_array_index (subscribers, SignalSubscriber, n));
      if (subscriber->user_data_free_func != NULL)
        subscriber->user_data_free_func (subscriber->user_data);
    }

  g_array_free (subscribers, TRUE);
}

/* ---------------------------------------------------------------------------------------------------- */

/* TODO: mark this function as (skip) so it won't show up in bindings
 *       cf. http://bugzilla.gnome.org/show_bug.cgi?id=556628
 */

typedef struct
{
  GDBusSignalCallback callback;
  gpointer user_data;
  GDestroyNotify user_data_free_func;
} CSignalSubscriber;

static void
c_signal_cb (GDBusConnection *connection,
             DBusMessage     *message,
             gpointer         user_data)
{
  CSignalSubscriber *subscriber = user_data;
  GPtrArray *p;
  DBusMessageIter iter;
  guint n;

  p = g_ptr_array_new_with_free_func (g_object_unref);

  dbus_message_iter_init (message, &iter);

  n = 0;
  while (dbus_message_iter_get_arg_type (&iter) != DBUS_TYPE_INVALID)
    {
      GValue value = {0};
      GError *error;
      char *arg_signature;
      GDBusVariant *variant;

      error = NULL;
      if (!g_dbus_c_type_mapping_get_value_from_iter (&iter,
                                                      &value,
                                                      &error))
        {
          g_warning ("Error getting argument %d from %s signal: %s",
                     n,
                     dbus_message_get_member (message),
                     error->message);
          g_error_free (error);
          goto out;
        }

      arg_signature = dbus_message_iter_get_signature (&iter);
      variant = _g_dbus_variant_new_for_gvalue (&value, arg_signature);
      dbus_free (arg_signature);

      g_value_unset (&value);

      g_ptr_array_add (p, variant);

      dbus_message_iter_next (&iter);
      n++;
    }

  subscriber->callback (connection,
                        dbus_message_get_member (message),
                        dbus_message_get_signature (message),
                        p,
                        subscriber->user_data);

 out:
  g_ptr_array_free (p, TRUE);
}

static void
free_c_subscriber (CSignalSubscriber *subscriber)
{
  if (subscriber->user_data_free_func != NULL)
    subscriber->user_data_free_func (subscriber->user_data);
  g_free (subscriber);
}

/**
 * g_dbus_connection_signal_subscribe:
 * @connection: A #GDBusConnection.
 * @sender: Sender name to match on. Must be either <literal>org.freedesktop.DBus</literal> (for listening to signals from the message bus daemon) or a unique name or %NULL to listen from all senders.
 * @interface_name: D-Bus interface name to match on or %NULL to match on all interfaces.
 * @member: D-Bus signal name to match on or %NULL to match on all signals.
 * @object_path: Object path to match on or %NULL to match on all object paths.
 * @arg0: Contents of first string argument to match on or %NULL to match on all kinds of arguments.
 * @callback: Callback to invoke when there is a signal matching the requested data.
 * @user_data: User data to pass to @callback.
 * @user_data_free_func: Function to free @user_data when subscription is removed or %NULL.
 *
 * Subscribes to signals on @connection and invokes @callback with the
 * contents of the signal whenever it is received.
 *
 * It is considered a programming error to use this function if
 * @connection has been disconnected.
 *
 * Note that if @sender is not <literal>org.freedesktop.DBus</literal> (for listening to signals from the
 * message bus daemon), then it needs to be a unique bus name or %NULL (for listening to signals from any
 * name) - you cannot pass a name like <literal>com.example.MyApp</literal>.
 * Use e.g. g_bus_watch_name() to find the unique name for the owner of the name you are interested in. Also note
 * that this function does not remove a subscription if @sender vanishes from the bus. You have to manually
 * call g_dbus_connection_signal_unsubscribe() to remove a subscription.
 *
 * Returns: A subscription identifier that can be used with g_dbus_connection_signal_unsubscribe().
 **/
guint
g_dbus_connection_signal_subscribe (GDBusConnection     *connection,
                                    const gchar         *sender,
                                    const gchar         *interface_name,
                                    const gchar         *member,
                                    const gchar         *object_path,
                                    const gchar         *arg0,
                                    GDBusSignalCallback  callback,
                                    gpointer             user_data,
                                    GDestroyNotify       user_data_free_func)
{
  CSignalSubscriber *subscriber;
  guint subscription_id;

  g_return_val_if_fail (G_IS_DBUS_CONNECTION (connection), 0);
  g_return_val_if_fail (!g_dbus_connection_get_is_disconnected (connection), 0);
  g_return_val_if_fail (callback != NULL, 0);

  subscriber = g_new0 (CSignalSubscriber, 1);
  subscriber->callback = callback;
  subscriber->user_data = user_data;
  subscriber->user_data_free_func = user_data_free_func;

  subscription_id = g_dbus_connection_dbus_1_signal_subscribe (connection,
                                                               sender,
                                                               interface_name,
                                                               member,
                                                               object_path,
                                                               arg0,
                                                               c_signal_cb,
                                                               subscriber,
                                                               (GDestroyNotify) free_c_subscriber);

  if (subscription_id == 0)
    {
      g_free (subscriber);
      goto out;
    }

 out:
  return subscription_id;
}

/* TODO: mark this function as (skip) so it won't show up in bindings
 *       cf. http://bugzilla.gnome.org/show_bug.cgi?id=556628
 */

/**
 * g_dbus_connection_signal_unsubscribe:
 * @connection: A #GDBusConnection.
 * @subscription_id: A subscription id obtained from g_dbus_connection_signal_subscribe().
 *
 * Unsubscribes from signals.
 **/
void
g_dbus_connection_signal_unsubscribe (GDBusConnection     *connection,
                                      guint                subscription_id)
{
  /* TODO: free CSignalSubscriber */
  g_dbus_connection_dbus_1_signal_unsubscribe (connection, subscription_id);
}

/* ---------------------------------------------------------------------------------------------------- */

#define __G_DBUS_CONNECTION_C__
#include "gdbusaliasdef.c"
