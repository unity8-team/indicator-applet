/*
A library to allow applictions to provide simple indications of
information to be displayed to users of the application through the
interface shell.

Copyright 2009 Canonical Ltd.

Authors:
    Ted Gould <ted@canonical.com>

This program is free software: you can redistribute it and/or modify it 
under the terms of either or both of the following licenses:

1) the GNU Lesser General Public License version 3, as published by the 
Free Software Foundation; and/or
2) the GNU Lesser General Public License version 2.1, as published by 
the Free Software Foundation.

This program is distributed in the hope that it will be useful, but 
WITHOUT ANY WARRANTY; without even the implied warranties of 
MERCHANTABILITY, SATISFACTORY QUALITY or FITNESS FOR A PARTICULAR 
PURPOSE.  See the applicable version of the GNU Lesser General Public 
License for more details.

You should have received a copy of both the GNU Lesser General Public 
License version 3 and version 2.1 along with this program.  If not, see 
<http://www.gnu.org/licenses/>
*/

#include "listener.h"
#include "listener-marshal.h"
#include <dbus/dbus-glib-bindings.h>
#include "dbus-indicate-client.h"
#include "dbus-listener-client.h"
#include "interests-priv.h"

/* Errors */
enum {
	LAST_ERROR
};

/* Signals */
enum {
	INDICATOR_ADDED,
	INDICATOR_REMOVED,
	INDICATOR_MODIFIED,
	SERVER_ADDED,
	SERVER_REMOVED,
	LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

struct _IndicateListenerServer {
	gchar * name;
	DBusGProxy * proxy;
	DBusGConnection * connection;
	gboolean interests[INDICATE_INTEREST_LAST];
};

struct _IndicateListenerIndicator {
	guint id;
};

typedef struct _IndicateListenerPrivate IndicateListenerPrivate;
struct _IndicateListenerPrivate
{
	DBusGConnection * session_bus;
	DBusGConnection * system_bus;

	DBusGProxy * dbus_proxy_session;
	DBusGProxy * dbus_proxy_system;

	GList * proxies_working;
	GList * proxies_possible;

	GArray * proxy_todo;
	guint todo_idle;
};

#define INDICATE_LISTENER_GET_PRIVATE(o) \
		(G_TYPE_INSTANCE_GET_PRIVATE ((o), INDICATE_TYPE_LISTENER, IndicateListenerPrivate))

typedef struct {
	DBusGProxy * proxy;
	DBusGProxy * property_proxy;
	DBusGConnection * connection;
	gchar * name;
	gchar * type;
	IndicateListener * listener;
	GHashTable * indicators;

	IndicateListenerServer server;
} proxy_t;

static gint
proxy_t_equal (gconstpointer pa, gconstpointer pb)
{
	proxy_t * a = (proxy_t *)pa; proxy_t * b = (proxy_t *)pb;

	if (a->connection == b->connection) {
		return g_strcmp0(a->name, b->name);
	} else {
		/* we're only using this for equal, not sorting */
		return 1;
	}
}

typedef struct {
	DBusGConnection * bus;
	gchar * name;
	gboolean startup;
} proxy_todo_t;

G_DEFINE_TYPE (IndicateListener, indicate_listener, G_TYPE_OBJECT);

/* Prototypes */
static void indicate_listener_finalize (GObject * obj);
static void dbus_owner_change (DBusGProxy * proxy, const gchar * name, const gchar * prev, const gchar * new, IndicateListener * listener);
static void proxy_struct_destroy (gpointer data);
static void build_todo_list_cb (DBusGProxy * proxy, char ** names, GError * error, void * data);
static void todo_list_add (const gchar * name, DBusGProxy * proxy, IndicateListener * listener, gboolean startup);
static gboolean todo_idle (gpointer data);
static void get_type_cb (IndicateListener * listener, IndicateListenerServer * server, gchar * type, gpointer data);
static void proxy_server_added (DBusGProxy * proxy, const gchar * type, proxy_t * proxyt);
static void proxy_indicator_added (DBusGProxy * proxy, guint id, const gchar * type, proxy_t * proxyt);
static void proxy_indicator_removed (DBusGProxy * proxy, guint id, const gchar * type, proxy_t * proxyt);
static void proxy_indicator_modified (DBusGProxy * proxy, guint id, const gchar * type, proxy_t * proxyt);
static void proxy_get_indicator_list (DBusGProxy * proxy, GArray * indicators, GError * error, gpointer data);
static void proxy_get_indicator_type (DBusGProxy * proxy, gchar * type, GError * error, gpointer data);
static void proxy_indicators_free (gpointer data);

/* DBus interface */
gboolean _indicate_listener_get_indicator_servers (IndicateListener * listener, GList * servers);

/* Need the above prototypes */
#include "dbus-listener-server.h"

/* Code */
static void
indicate_listener_class_init (IndicateListenerClass * class)
{
	/* g_debug("Listener Class Initialized"); */
	GObjectClass * gobj;
	gobj = G_OBJECT_CLASS(class);

	g_type_class_add_private (class, sizeof (IndicateListenerPrivate));

	gobj->finalize = indicate_listener_finalize;

	signals[INDICATOR_ADDED] = g_signal_new(INDICATE_LISTENER_SIGNAL_INDICATOR_ADDED,
	                                        G_TYPE_FROM_CLASS (class),
	                                        G_SIGNAL_RUN_LAST,
	                                        G_STRUCT_OFFSET (IndicateListenerClass, indicator_added),
	                                        NULL, NULL,
	                                        _indicate_listener_marshal_VOID__BOXED_POINTER_STRING,
	                                        G_TYPE_NONE, 3, INDICATE_TYPE_LISTENER_SERVER, G_TYPE_POINTER, G_TYPE_STRING);
	signals[INDICATOR_REMOVED] = g_signal_new(INDICATE_LISTENER_SIGNAL_INDICATOR_REMOVED,
	                                        G_TYPE_FROM_CLASS (class),
	                                        G_SIGNAL_RUN_LAST,
	                                        G_STRUCT_OFFSET (IndicateListenerClass, indicator_removed),
	                                        NULL, NULL,
	                                        _indicate_listener_marshal_VOID__BOXED_POINTER_STRING,
	                                        G_TYPE_NONE, 3, INDICATE_TYPE_LISTENER_SERVER, G_TYPE_POINTER, G_TYPE_STRING);
	signals[INDICATOR_MODIFIED] = g_signal_new(INDICATE_LISTENER_SIGNAL_INDICATOR_MODIFIED,
	                                        G_TYPE_FROM_CLASS (class),
	                                        G_SIGNAL_RUN_LAST,
	                                        G_STRUCT_OFFSET (IndicateListenerClass, indicator_modified),
	                                        NULL, NULL,
	                                        _indicate_listener_marshal_VOID__BOXED_POINTER_STRING_STRING,
	                                        G_TYPE_NONE, 4, INDICATE_TYPE_LISTENER_SERVER, G_TYPE_POINTER, G_TYPE_STRING, G_TYPE_STRING);
	signals[SERVER_ADDED] = g_signal_new(INDICATE_LISTENER_SIGNAL_SERVER_ADDED,
	                                        G_TYPE_FROM_CLASS (class),
	                                        G_SIGNAL_RUN_LAST,
	                                        G_STRUCT_OFFSET (IndicateListenerClass, server_added),
	                                        NULL, NULL,
	                                        _indicate_listener_marshal_VOID__BOXED_STRING,
	                                        G_TYPE_NONE, 2, INDICATE_TYPE_LISTENER_SERVER, G_TYPE_STRING);
	signals[SERVER_REMOVED] = g_signal_new(INDICATE_LISTENER_SIGNAL_SERVER_REMOVED,
	                                        G_TYPE_FROM_CLASS (class),
	                                        G_SIGNAL_RUN_LAST,
	                                        G_STRUCT_OFFSET (IndicateListenerClass, server_removed),
	                                        NULL, NULL,
	                                        _indicate_listener_marshal_VOID__BOXED_STRING,
	                                        G_TYPE_NONE, 2, INDICATE_TYPE_LISTENER_SERVER, G_TYPE_STRING);

	dbus_g_object_register_marshaller(_indicate_listener_marshal_VOID__UINT_STRING,
	                                  G_TYPE_NONE,
	                                  G_TYPE_UINT,
	                                  G_TYPE_STRING,
	                                  G_TYPE_INVALID);

	return;
}

static void
indicate_listener_init (IndicateListener * listener)
{
	/* g_debug("Listener Object Initialized"); */
	IndicateListenerPrivate * priv = INDICATE_LISTENER_GET_PRIVATE(listener);
	GError *error = NULL;

	/* Get the buses */
	priv->session_bus = dbus_g_bus_get(DBUS_BUS_SESSION, &error);
	if (error != NULL) {
		g_error("Unable to get session bus: %s", error->message);
		g_error_free(error);
		return;
	}

	priv->system_bus = dbus_g_bus_get(DBUS_BUS_SYSTEM, &error);
	if (error != NULL) {
		g_error("Unable to get system bus: %s", error->message);
		g_error_free(error);
		return;
	}

	/* Set up the DBUS service proxies */
	priv->dbus_proxy_session = dbus_g_proxy_new_for_name_owner (priv->session_bus,
	                                                                DBUS_SERVICE_DBUS,
	                                                                DBUS_PATH_DBUS,
	                                                                DBUS_INTERFACE_DBUS,
	                                                                &error);
	if (error != NULL) {
		g_error("Unable to get dbus proxy on session bus: %s", error->message);
		g_error_free(error);
		return;
	}

	priv->dbus_proxy_system = dbus_g_proxy_new_for_name_owner (priv->system_bus,
	                                                               DBUS_SERVICE_DBUS,
	                                                               DBUS_PATH_DBUS,
	                                                               DBUS_INTERFACE_DBUS,
	                                                               &error);
	if (error != NULL) {
		g_error("Unable to get dbus proxy on system bus: %s", error->message);
		g_error_free(error);
		return;
	}

	/* Set up name change signals */
	dbus_g_proxy_add_signal(priv->dbus_proxy_session, "NameOwnerChanged",
	                        G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
	                        G_TYPE_INVALID);
	dbus_g_proxy_connect_signal(priv->dbus_proxy_session, "NameOwnerChanged",
	                            G_CALLBACK(dbus_owner_change), listener, NULL);
	dbus_g_proxy_add_signal(priv->dbus_proxy_system, "NameOwnerChanged",
	                        G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
	                        G_TYPE_INVALID);
	dbus_g_proxy_connect_signal(priv->dbus_proxy_system, "NameOwnerChanged",
	                            G_CALLBACK(dbus_owner_change), listener, NULL);

	/* Initialize Data structures */
	priv->proxies_working = NULL;
	priv->proxies_possible = NULL;

	/* TODO: Look at some common scenarios and find out how to make this sized */
	priv->proxy_todo = g_array_new(FALSE, TRUE, sizeof(proxy_todo_t));
	priv->todo_idle = 0;

	/*            WARNING              */
	/* Starting massive asynchronisity */
	/*                                 */

	/* Build todo list */
	org_freedesktop_DBus_list_names_async (priv->dbus_proxy_session, build_todo_list_cb, listener);
	org_freedesktop_DBus_list_names_async (priv->dbus_proxy_system, build_todo_list_cb, listener);

	return;
}

static void
indicate_listener_finalize (GObject * obj)
{
	IndicateListener * listener = INDICATE_LISTENER(obj);

	return;
}

IndicateListener *
indicate_listener_new (void)
{
	g_warning("Creating a new listener is generally discouraged, please use indicate_listener_ref_default");

	IndicateListener * listener;
	listener = g_object_new(INDICATE_TYPE_LISTENER, NULL);
	return listener;
}

static IndicateListener * default_indicate_listener = NULL;

IndicateListener *
indicate_listener_ref_default (void)
{
	if (default_indicate_listener != NULL) {
		g_object_ref(default_indicate_listener);
	} else {
		default_indicate_listener = g_object_new(INDICATE_TYPE_LISTENER, NULL);
		g_object_add_weak_pointer(G_OBJECT(default_indicate_listener),
		                          (gpointer *)&default_indicate_listener);
	}

	return default_indicate_listener;
}

static void
dbus_owner_change (DBusGProxy * proxy, const gchar * name, const gchar * prev, const gchar * new, IndicateListener * listener)
{
	IndicateListenerPrivate * priv = INDICATE_LISTENER_GET_PRIVATE(listener);

	DBusGConnection * bus;
	gchar * bus_name;
	if (proxy == priv->dbus_proxy_system) {
		bus = priv->system_bus;
		bus_name = "system";
	} else {
		bus = priv->session_bus;
		bus_name = "session";
	}

	/* g_debug("Name change on %s bus: '%s' from '%s' to '%s'", bus_name, name, prev, new); */

	if (prev != NULL && prev[0] == '\0') {
		todo_list_add(name, proxy, listener, FALSE);
	}
	if (new != NULL && new[0] == '\0') {
		proxy_t searchitem;
		searchitem.connection = bus;
		searchitem.name = (gchar *)name; /* Droping const, not that it isn't, but to remove the warning */

		GList * proxyt_item;
		proxyt_item = g_list_find_custom(priv->proxies_working, &searchitem, proxy_t_equal);
		if (proxyt_item != NULL) {
			proxy_struct_destroy((proxy_t *)proxyt_item->data);
			priv->proxies_working = g_list_remove(priv->proxies_working, proxyt_item->data);
		}
		proxyt_item = g_list_find_custom(priv->proxies_possible, &searchitem, proxy_t_equal);
		if (proxyt_item != NULL) {
			proxy_struct_destroy((proxy_t *)proxyt_item->data);
			priv->proxies_possible = g_list_remove(priv->proxies_possible, proxyt_item->data);
		}
	}

	return;
}

static void
proxy_struct_destroy_indicators (gpointer key, gpointer value, gpointer data)
{
	gchar * type = (gchar *)key;
	GHashTable * indicators = (GHashTable *)value;
	proxy_t * proxy_data = data;

	GList * keys = g_hash_table_get_keys(indicators);
	GList * indicator;
	for (indicator = keys; indicator != NULL; indicator = indicator->next) {
		guint id = (guint)indicator->data;
		g_signal_emit(proxy_data->listener, signals[INDICATOR_REMOVED], 0, &proxy_data->server, GUINT_TO_POINTER(id), type, TRUE);
	}
	g_list_free(keys);

	g_hash_table_remove_all(indicators);
	return;
}

static void
proxy_struct_destroy (gpointer data)
{
	proxy_t * proxy_data = data;

	/* TODO: Clear the indicators by signaling */
	if (proxy_data->indicators != NULL) {
		g_hash_table_foreach(proxy_data->indicators,
							 proxy_struct_destroy_indicators,
							 proxy_data);
		g_hash_table_remove_all(proxy_data->indicators);

		g_signal_emit(proxy_data->listener, signals[SERVER_REMOVED], 0, &proxy_data->server, proxy_data->type, TRUE);
		proxy_data->indicators = NULL;
	}

	if (DBUS_IS_G_PROXY(proxy_data->property_proxy)) {
		g_object_unref(G_OBJECT(proxy_data->property_proxy));
	}

	if (DBUS_IS_G_PROXY(proxy_data->proxy)) {
		g_object_unref(G_OBJECT(proxy_data->proxy));
	}

	g_free(proxy_data->name);
	if (proxy_data->type != NULL) {
		g_free(proxy_data->type);
	}
	g_free(proxy_data);

	return;
}

static void
build_todo_list_cb (DBusGProxy * proxy, char ** names, GError * error, void * data)
{
	IndicateListener * listener = INDICATE_LISTENER(data);

	if (error != NULL) {
		g_warning("Unable to get names: %s", error->message);
		return;
	}

	guint i = 0;
	for (i = 0; names[i] != NULL; i++) {
		todo_list_add(names[i], proxy, listener, TRUE);
	}

	return;
}

static void
todo_list_add (const gchar * name, DBusGProxy * proxy, IndicateListener * listener, gboolean startup)
{
	if (name == NULL || name[0] != ':') {
		return;
	}

	IndicateListenerPrivate * priv = INDICATE_LISTENER_GET_PRIVATE(listener);

	DBusGConnection * bus;
	gchar * bus_name;
	if (proxy == priv->dbus_proxy_system) {
		bus = priv->system_bus;
		bus_name = "system";
	} else {
		bus = priv->session_bus;
		bus_name = "session";
	}
	/* g_debug ("Adding on %s bus: %s", bus_name, name); */

	proxy_todo_t todo;
	todo.name = g_strdup(name);
	todo.bus  = bus;
	todo.startup = startup;

	g_array_append_val(priv->proxy_todo, todo);

	if (priv->todo_idle == 0) {
		priv->todo_idle = g_idle_add(todo_idle, listener);
	}

	return;
}

gboolean
todo_idle (gpointer data)
{
	IndicateListener * listener = INDICATE_LISTENER(data);
	if (listener == NULL) {
		g_error("Listener got lost in todo_idle");
		return FALSE;
	}

	IndicateListenerPrivate * priv = INDICATE_LISTENER_GET_PRIVATE(listener);

	if (priv->proxy_todo->len == 0) {
		/* Basically if we have no todo, we need to stop running.  This
		 * is done this way to make the function error handling simpler
		 * and results in an extra run */
		priv->todo_idle = 0;
		return FALSE;
	}

	proxy_todo_t * todo = &g_array_index(priv->proxy_todo, proxy_todo_t, priv->proxy_todo->len - 1);

	proxy_t * proxyt = g_new0(proxy_t, 1);
	proxyt->name = todo->name;
	proxyt->type = NULL;
	proxyt->proxy = dbus_g_proxy_new_for_name(todo->bus,
	                                          proxyt->name,
	                                          "/org/freedesktop/indicate",
	                                          "org.freedesktop.indicator");
	proxyt->property_proxy = NULL;
	proxyt->listener = listener;
	proxyt->indicators = NULL;
	proxyt->connection = todo->bus;
	proxyt->server.name = todo->name;
	proxyt->server.proxy = proxyt->proxy;
	proxyt->server.connection = proxyt->connection;

	priv->proxy_todo = g_array_remove_index(priv->proxy_todo, priv->proxy_todo->len - 1);

	if (proxyt->proxy == NULL) {
		g_warning("Unable to create proxy for %s", proxyt->name);
		return TRUE;
	}

	dbus_g_proxy_add_signal(proxyt->proxy, "ServerShow",
	                        G_TYPE_STRING, G_TYPE_INVALID);
	dbus_g_proxy_connect_signal(proxyt->proxy, "ServerShow",
	                            G_CALLBACK(proxy_server_added), proxyt, NULL);

	priv->proxies_possible = g_list_prepend(priv->proxies_possible, proxyt);

	/* I think that we need to have this as there is a race
	 * condition here.  If someone comes on the bus and we get
	 * that message, but before we set up the handler for the ServerShow
	 * signal it gets sent, we wouldn't get it.  So then we would
	 * miss an indicator server coming on the bus.  I'd like to not
	 * generate a warning in every app with DBus though. */
	indicate_listener_server_get_type(listener, &proxyt->server, get_type_cb, proxyt);

	return TRUE;
}

static void
get_type_cb (IndicateListener * listener, IndicateListenerServer * server, gchar * type, gpointer data)
{
	if (type == NULL) {
		/* This is usually caused by an error getting the type,
		 * which would mean that this isn't an indicator server */
		return;
	}

	proxy_t * proxyt = (proxy_t *)data;

	proxy_server_added (proxyt->proxy, type, proxyt);
	org_freedesktop_indicator_get_indicator_list_async(proxyt->proxy, proxy_get_indicator_list, proxyt);

	return;
}

typedef struct {
	guint id;
	proxy_t * proxyt;
} indicator_type_t;

static void
proxy_get_indicator_list (DBusGProxy * proxy, GArray * indicators, GError * error, gpointer data)
{
	if (error != NULL) {
		return;
	}

	proxy_t * proxyt = (proxy_t *)data;

	int i;
	for (i = 0; i < indicators->len; i++) {
		indicator_type_t * itt = g_new(indicator_type_t, 1);
		itt->id = g_array_index(indicators, guint, i);
		itt->proxyt = proxyt;

		org_freedesktop_indicator_get_indicator_property_async(proxyt->proxy, itt->id, "type", proxy_get_indicator_type, itt);
	}

	return;
}

static void
proxy_get_indicator_type (DBusGProxy * proxy, gchar * type, GError * error, gpointer data)
{
	if (error != NULL) {
		g_warning("Get Indicator Type returned error: %s", error->message);
		return;
	}

	indicator_type_t * itt = (indicator_type_t *)data;
	guint id = itt->id;
	proxy_t * proxyt = itt->proxyt;

	g_free(itt);

	return proxy_indicator_added(proxy, id, type, proxyt);
}

static void
proxy_server_added (DBusGProxy * proxy, const gchar * type, proxy_t * proxyt)
{
	if (proxyt->indicators == NULL) {
		proxyt->indicators = g_hash_table_new_full(g_str_hash, g_str_equal,
		                                           g_free, proxy_indicators_free);
		/* Elevate to working */
		IndicateListenerPrivate * priv = INDICATE_LISTENER_GET_PRIVATE(proxyt->listener);

		GList * proxyt_item;
		proxyt_item = g_list_find_custom(priv->proxies_possible, proxyt, proxy_t_equal);
		if (proxyt_item != NULL) {
			priv->proxies_possible = g_list_remove(priv->proxies_possible, proxyt_item->data);
		}
		priv->proxies_working = g_list_prepend(priv->proxies_working, proxyt);

		dbus_g_proxy_add_signal(proxyt->proxy, "IndicatorAdded",
								G_TYPE_UINT, G_TYPE_STRING, G_TYPE_INVALID);
		dbus_g_proxy_connect_signal(proxyt->proxy, "IndicatorAdded",
									G_CALLBACK(proxy_indicator_added), proxyt, NULL);
		dbus_g_proxy_add_signal(proxyt->proxy, "IndicatorRemoved",
								G_TYPE_UINT, G_TYPE_STRING, G_TYPE_INVALID);
		dbus_g_proxy_connect_signal(proxyt->proxy, "IndicatorRemoved",
									G_CALLBACK(proxy_indicator_removed), proxyt, NULL);
		dbus_g_proxy_add_signal(proxyt->proxy, "IndicatorModified",
								G_TYPE_UINT, G_TYPE_STRING, G_TYPE_INVALID);
		dbus_g_proxy_connect_signal(proxyt->proxy, "IndicatorModified",
									G_CALLBACK(proxy_indicator_modified), proxyt, NULL);

		if (type != NULL) {
			if (proxyt->type != NULL) {
				g_free(proxyt->type);
			}
			proxyt->type = g_strdup(type);
		}

		g_signal_emit(proxyt->listener, signals[SERVER_ADDED], 0, &proxyt->server, proxyt->type, TRUE);
	}

	return;
}

static void
proxy_indicator_added (DBusGProxy * proxy, guint id, const gchar * type, proxy_t * proxyt)
{
	if (proxyt->indicators == NULL) {
		proxy_server_added (proxy, NULL, proxyt);
	}

	GHashTable * indicators = g_hash_table_lookup(proxyt->indicators, type);

	if (indicators == NULL) {
		indicators = g_hash_table_new(g_direct_hash, g_direct_equal);
		g_hash_table_insert(proxyt->indicators, g_strdup(type), indicators);
	}

	if (!g_hash_table_lookup(indicators, (gpointer)id)) {
		g_hash_table_insert(indicators, (gpointer)id, (gpointer)TRUE);
		g_signal_emit(proxyt->listener, signals[INDICATOR_ADDED], 0, &proxyt->server, GUINT_TO_POINTER(id), type, TRUE);
	}

	return;
}

static void
proxy_indicator_removed (DBusGProxy * proxy, guint id, const gchar * type, proxy_t * proxyt)
{
	if (proxyt->indicators == NULL) {
		g_warning("Oddly we had an indicator removed from an interface that we didn't think had indicators.");
		return;
	}

	GHashTable * indicators = g_hash_table_lookup(proxyt->indicators, type);
	if (indicators == NULL) {
		g_warning("Can not remove indicator %d of type '%s' as there are no indicators of that type on %s.", id, type, proxyt->name);
		return;
	}

	if (!g_hash_table_lookup(indicators, (gpointer)id)) {
		g_warning("No indicator %d of type '%s' on '%s'.", id, type, proxyt->name);
		return;
	}

	g_hash_table_remove(indicators, (gpointer)id);
	g_signal_emit(proxyt->listener, signals[INDICATOR_REMOVED], 0, &proxyt->server, GUINT_TO_POINTER(id), type, TRUE);

	return;
}

static void
proxy_indicator_modified (DBusGProxy * proxy, guint id, const gchar * property, proxy_t * proxyt)
{
	if (proxyt->indicators == NULL) {
		g_warning("Oddly we had an indicator modified from an interface that we didn't think had indicators.");
		return;
	}

	GList * keys = g_hash_table_get_keys(proxyt->indicators);
	GList * inc = NULL;
	gchar * type;

	for (inc = g_list_first(keys); inc != NULL; inc = g_list_next(inc)) {
		type = (gchar *)inc->data;

		GHashTable * indicators = g_hash_table_lookup(proxyt->indicators, type);
		if (indicators == NULL) continue; /* no indicators for this type?  Odd, but not an error */

		if (g_hash_table_lookup(indicators, (gpointer)id)) {
			break;
		}
	}

	if (inc == NULL) {
		g_warning("Can not modify indicator %d with property '%s' as there are no indicators with that id on %s.", id, property, proxyt->name);
		return;
	}

	g_signal_emit(proxyt->listener, signals[INDICATOR_MODIFIED], 0, proxyt->server, GUINT_TO_POINTER(id), type, property, TRUE);

	return;
}

static void
proxy_indicators_free (gpointer data)
{
	GHashTable * table = (GHashTable *)data;

	if (g_hash_table_size(table) != 0) {
		g_warning("Clearning a set of indicators that wasn't signaled!");
	}

	g_hash_table_unref(table);
	return;
}

typedef enum _get_property_type get_property_type;
enum _get_property_type {
	PROPERTY_TYPE_STRING,
	PROPERTY_TYPE_TIME,
	PROPERTY_TYPE_ICON
};

typedef struct _get_property_t get_property_t;
struct _get_property_t {
	GCallback cb;
	gpointer data;
	IndicateListener * listener;
	IndicateListenerServer * server;
	IndicateListenerIndicator * indicator;
	gchar * property;
	get_property_type type;
};

static void
get_property_cb (DBusGProxy *proxy, char * OUT_value, GError *error, gpointer userdata)
{
	get_property_t * get_property_data = (get_property_t *)userdata;

	if (error != NULL) {
		g_warning("Unable to get property data: %s", error->message);
		g_error_free(error);
		return;
	}

	switch (get_property_data->type) {
	case PROPERTY_TYPE_STRING: {
		indicate_listener_get_property_cb cb = (indicate_listener_get_property_cb)get_property_data->cb;
		cb(get_property_data->listener, get_property_data->server, get_property_data->indicator, get_property_data->property, OUT_value, get_property_data->data);
		break;
	}
	case PROPERTY_TYPE_ICON: {
		indicate_listener_get_property_icon_cb cb = (indicate_listener_get_property_icon_cb)get_property_data->cb;

		/* There is no icon */
		if (OUT_value == NULL || OUT_value[0] == '\0') {
			break;
		}

		gsize length = 0;
		guchar * icondata = g_base64_decode(OUT_value, &length);
		
		GInputStream * input = g_memory_input_stream_new_from_data(icondata, length, NULL);
		if (input == NULL) {
			g_warning("Cound not create input stream from icon property data");
			g_free(icondata);
			break;
		}

		GError * error = NULL;
		GdkPixbuf * icon = gdk_pixbuf_new_from_stream(input, NULL, &error);
		if (icon != NULL) {
			cb(get_property_data->listener, get_property_data->server, get_property_data->indicator, get_property_data->property, icon, get_property_data->data);
		}

		if (error != NULL) {
			g_warning("Unable to build Pixbuf from icon data: %s", error->message);
			g_error_free(error);
		}

		error = NULL;
		g_input_stream_close(input, NULL, &error);
		if (error != NULL) {
			g_warning("Unable to close input stream: %s", error->message);
			g_error_free(error);
		}
		g_free(icondata);
		break;
	}
	case PROPERTY_TYPE_TIME: {
		indicate_listener_get_property_time_cb cb = (indicate_listener_get_property_time_cb)get_property_data->cb;
		GTimeVal time;
		if (g_time_val_from_iso8601(OUT_value, &time)) {
			cb(get_property_data->listener, get_property_data->server, get_property_data->indicator, get_property_data->property, &time, get_property_data->data);
		}
		break;
	}
	}

	g_free(get_property_data->property);
	g_free(get_property_data);

	return;
};

static void
get_property_helper (IndicateListener * listener, IndicateListenerServer * server, IndicateListenerIndicator * indicator, gchar * property, GCallback callback, gpointer data, get_property_type prop_type)
{
	/* g_debug("get_property_helper: %s %d", property, prop_type); */
	/* TODO: Do we need to somehow refcount the server/indicator while we're waiting on this? */
	get_property_t * get_property_data = g_new(get_property_t, 1);
	get_property_data->cb = callback;
	get_property_data->data = data;
	get_property_data->listener = listener;
	get_property_data->server = server;
	get_property_data->indicator = indicator;
	get_property_data->property = g_strdup(property);
	get_property_data->type = prop_type;
	
	org_freedesktop_indicator_get_indicator_property_async (server->proxy , INDICATE_LISTENER_INDICATOR_ID(indicator), property, get_property_cb, get_property_data);
	return;
}

void
indicate_listener_get_property (IndicateListener * listener, IndicateListenerServer * server, IndicateListenerIndicator * indicator, gchar * property, indicate_listener_get_property_cb callback, gpointer data)
{
	return get_property_helper(listener, server, indicator, property, G_CALLBACK(callback), data, PROPERTY_TYPE_STRING);
}

void
indicate_listener_get_property_time (IndicateListener * listener, IndicateListenerServer * server, IndicateListenerIndicator * indicator, gchar * property, indicate_listener_get_property_time_cb callback, gpointer data)
{
	return get_property_helper(listener, server, indicator, property, G_CALLBACK(callback), data, PROPERTY_TYPE_TIME);
}

void
indicate_listener_get_property_icon (IndicateListener * listener, IndicateListenerServer * server, IndicateListenerIndicator * indicator, gchar * property, indicate_listener_get_property_icon_cb callback, gpointer data)
{
	return get_property_helper(listener, server, indicator, property, G_CALLBACK(callback), data, PROPERTY_TYPE_ICON);
}

gboolean
_indicate_listener_get_indicator_servers (IndicateListener * listener, GList * servers)
{


}

static void 
listener_display_cb (DBusGProxy *proxy, GError *error, gpointer userdata)
{
	if (error != NULL) {
		g_warning("Listener display caused an error: %s", error->message);
	}
	return;
}

void
indicate_listener_display (IndicateListener * listener, IndicateListenerServer * server, IndicateListenerIndicator * indicator)
{
	org_freedesktop_indicator_show_indicator_to_user_async (server->proxy, INDICATE_LISTENER_INDICATOR_ID(indicator), listener_display_cb, NULL);

	return;
}

typedef struct {
	IndicateListener * listener;
	IndicateListenerServer * server;
	indicate_listener_get_server_property_cb cb;
	gpointer data;
} property_cb_t;

static void
property_cb (DBusGProxy * proxy, DBusGProxyCall * call, void * data)
{
	/* g_debug("Callback for property %s %s %s", dbus_g_proxy_get_bus_name(proxy), dbus_g_proxy_get_path(proxy), dbus_g_proxy_get_interface(proxy)); */
	property_cb_t * propertyt = data;
	GError * error = NULL;

	GValue property = {0};

	dbus_g_proxy_end_call(proxy, call, &error, G_TYPE_VALUE, &property, G_TYPE_INVALID);
	if (error != NULL) {
		/* g_warning("Unable to get property: %s", error->message); */
		g_error_free(error);
		g_free(propertyt);
		return;
	}

	if (!G_VALUE_HOLDS_STRING(&property)) {
		g_warning("Property returned is not a string!");
		g_free(propertyt);
		return;
	}

	IndicateListener * listener = propertyt->listener;
	IndicateListenerServer * server = propertyt->server;
	indicate_listener_get_server_property_cb cb = propertyt->cb;
	gpointer cb_data = propertyt->data;

	g_free(propertyt);

	gchar * propstr = g_value_dup_string(&property);

	/* g_debug("\tProperty value: %s", propstr); */

	return cb(listener, server, propstr, cb_data);
}

static void
get_server_property (IndicateListener * listener, IndicateListenerServer * server, indicate_listener_get_server_property_cb callback, const gchar * property_name, gpointer data)
{
	/* g_debug("Setting up callback for property %s on %s", property_name, INDICATE_LISTENER_SERVER_DBUS_NAME(server)); */
	IndicateListenerPrivate * priv = INDICATE_LISTENER_GET_PRIVATE(listener);

	proxy_t searchitem;
	searchitem.name = server->name;
	searchitem.connection = server->connection;

	GList * proxyitem = g_list_find_custom(priv->proxies_possible, &searchitem, proxy_t_equal);
	if (proxyitem == NULL) {
		proxyitem = g_list_find_custom(priv->proxies_working, &searchitem, proxy_t_equal);
	}

	if (proxyitem == NULL) {
		g_warning("Can not find a proxy for the server at all.");
		return;
	}

	proxy_t * proxyt = (proxy_t *)proxyitem->data;

	if (proxyt->property_proxy == NULL) {
		proxyt->property_proxy = dbus_g_proxy_new_for_name(proxyt->connection,
		                                                   proxyt->name,
		                                                   "/org/freedesktop/indicate",
		                                                   DBUS_INTERFACE_PROPERTIES);
	}

	property_cb_t * localdata = g_new(property_cb_t, 1);
	localdata->listener = listener;
	localdata->server = server;
	localdata->cb = callback;
	localdata->data = data;

	dbus_g_proxy_begin_call (proxyt->property_proxy,
	                         "Get",
	                         property_cb,
	                         localdata,
	                         NULL,
	                         G_TYPE_STRING, "org.freedesktop.indicator",
	                         G_TYPE_STRING, property_name,
	                         G_TYPE_INVALID, G_TYPE_VALUE, G_TYPE_INVALID);

	return;
}

void
indicate_listener_server_get_type (IndicateListener * listener, IndicateListenerServer * server, indicate_listener_get_server_property_cb callback, gpointer data)
{
	return get_server_property(listener, server, callback, "type", data);
}

void
indicate_listener_server_get_desktop (IndicateListener * listener, IndicateListenerServer * server, indicate_listener_get_server_property_cb callback, gpointer data)
{
	return get_server_property(listener, server, callback, "desktop", data);
}

const gchar *
indicate_listener_server_get_dbusname (IndicateListenerServer * server)
{
	if (server == NULL) return NULL;
	return server->name;
}

guint
indicate_listener_indicator_get_id (IndicateListenerIndicator * indicator)
{
	return GPOINTER_TO_UINT(indicator);
}

static const gchar *
interest_to_string (IndicateInterests interest)
{
	switch (interest) {
	case INDICATE_INTEREST_SERVER_DISPLAY:
		return INDICATE_INTEREST_STRING_SERVER_DISPLAY;
	case INDICATE_INTEREST_SERVER_SIGNAL:
		return INDICATE_INTEREST_STRING_SERVER_SIGNAL;
	case INDICATE_INTEREST_INDICATOR_DISPLAY:
		return INDICATE_INTEREST_STRING_INDICATOR_DISPLAY;
	case INDICATE_INTEREST_INDICATOR_SIGNAL:
		return INDICATE_INTEREST_STRING_INDICATOR_SIGNAL;
	case INDICATE_INTEREST_INDICATOR_COUNT:
		return INDICATE_INTEREST_STRING_INDICATOR_COUNT;
	default:
		return "";
	}
}

static void
interest_cb (DBusGProxy *proxy, GError *error, gpointer userdata)
{
	if (error != NULL) {
		g_warning("Unable to configure interest.");
	}

	return;
}

void
indicate_listener_server_show_interest (IndicateListener * listener, IndicateListenerServer * server, IndicateInterests interest)
{
	if (!server->interests[interest]) {
		org_freedesktop_indicator_show_interest_async (server->proxy, interest_to_string(interest), interest_cb, server);
		server->interests[interest] = TRUE;
	}
	return;
}

void
indicate_listener_server_remove_interest (IndicateListener * listener, IndicateListenerServer * server, IndicateInterests interest)
{
	if (server->interests[interest]) {
		org_freedesktop_indicator_remove_interest_async (server->proxy, interest_to_string(interest), interest_cb, server);
		server->interests[interest] = FALSE;
	}
	return;
}

gboolean
indicate_listener_server_check_interest (IndicateListener * listener, IndicateListenerServer * server, IndicateInterests interest)
{
	return server->interests[interest];
}

GType
indicate_listener_server_get_gtype (void)
{
  static GType our_type = 0;
  
  if (our_type == 0)
    our_type = g_boxed_type_register_static ("IndicateListenerServer",
					     (GBoxedCopyFunc) indicate_listener_server_copy,
					     (GBoxedFreeFunc) indicate_listener_server_free);

  return our_type;
}

IndicateListenerServer *
indicate_listener_server_copy (const IndicateListenerServer *listener_server)
{
        return listener_server;
}

void
indicate_listener_server_free (IndicateListenerServer *listener_server)
{
}
