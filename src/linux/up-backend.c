/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2010 Richard Hughes <richard@hughsie.com>
 *
 * Licensed under the GNU General Public License Version 2
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <sys/wait.h>
#include <glib/gi18n.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <gudev/gudev.h>

#include "up-backend.h"
#include "up-daemon.h"
#include "up-device.h"
#include "up-device-kbd-backlight.h"

#include "up-enumerator-udev.h"

#include "up-bt-gatt.h"

#include "up-device-supply.h"
#include "up-device-wup.h"
#include "up-device-hid.h"
#include "up-device-bluez.h"
#include "up-input.h"
#include "up-config.h"
#ifdef HAVE_IDEVICE
#include "up-device-idevice.h"
#endif /* HAVE_IDEVICE */

static void	up_backend_class_init	(UpBackendClass	*klass);
static void	up_backend_init	(UpBackend		*backend);
static void	up_backend_finalize	(GObject		*object);

#define LOGIND_DBUS_NAME                       "org.freedesktop.login1"
#define LOGIND_DBUS_PATH                       "/org/freedesktop/login1"
#define LOGIND_DBUS_INTERFACE                  "org.freedesktop.login1.Manager"

struct UpBackendPrivate
{
	UpDaemon		*daemon;
	UpDeviceList		*device_list;
	GUdevClient		*gudev_client;
	UpInput			*lid_device;
	UpConfig		*config;
	GDBusProxy		*logind_proxy;
	guint                    logind_sleep_id;
	int                      logind_delay_inhibitor_fd;

	UpEnumerator		*udev_enum;

	/* BlueZ */
	guint			 bluez_watch_id;
	GDBusObjectManager	*bluez_client;
	// device path -> device GDBusObject
	GHashTable		*gatt_devices;
	// service path -> device GDBusObject
	GHashTable		*gatt_battery_services;
};

enum {
	SIGNAL_DEVICE_ADDED,
	SIGNAL_DEVICE_REMOVED,
	SIGNAL_LAST
};

static guint signals [SIGNAL_LAST] = { 0 };

G_DEFINE_TYPE_WITH_PRIVATE (UpBackend, up_backend, G_TYPE_OBJECT)

static void
input_switch_changed_cb (UpInput   *input,
			 gboolean   switch_value,
			 UpBackend *backend)
{
	up_daemon_set_lid_is_closed (backend->priv->daemon, switch_value);
}

static void
up_backend_uevent_signal_handler_cb (GUdevClient *client, const gchar *action,
				      GUdevDevice *device, gpointer user_data)
{
	UpBackend *backend = UP_BACKEND (user_data);
	g_autoptr(UpInput) input = NULL;

	if (backend->priv->lid_device)
		return;

	if (g_strcmp0 (action, "add") != 0)
		return;

	/* check if the input device is a lid */
	input = up_input_new ();
	if (up_input_coldplug (input, device)) {
		up_daemon_set_lid_is_present (backend->priv->daemon, TRUE);
		g_signal_connect (G_OBJECT (input), "switch-changed",
				  G_CALLBACK (input_switch_changed_cb), backend);
		up_daemon_set_lid_is_closed (backend->priv->daemon,
					     up_input_get_switch_value (input));

		backend->priv->lid_device = g_steal_pointer (&input);
	}
}

static UpDevice *
find_duplicate_device (UpBackend *backend,
		       UpDevice  *device)
{
	GPtrArray *array;
	g_autofree char *serial = NULL;
	UpDevice *ret = NULL;
	guint i;

	g_object_get (G_OBJECT (device), "serial", &serial, NULL);
	if (!serial)
		return NULL;

	array = up_device_list_get_array (backend->priv->device_list);
	for (i = 0; i < array->len; i++) {
		g_autofree char *s = NULL;
		UpDevice *d;

		d = UP_DEVICE (g_ptr_array_index (array, i));
		if (d == device)
			continue;
		g_object_get (G_OBJECT (d), "serial", &s, NULL);
		if (s && g_ascii_strcasecmp (s, serial) == 0) {
			ret = g_object_ref (d);
			break;
		}
	}
	g_ptr_array_unref (array);

	return ret;
}

/* Returns TRUE if the added_device should be visible */
static gboolean
update_added_duplicate_device (UpBackend *backend,
			       UpDevice  *added_device)
{
	g_autoptr(UpDevice) other_device = NULL;
	UpDevice *bluez_device = NULL;
	UpDevice *unreg_device = NULL;
	g_autofree char *serial = NULL;

	other_device = find_duplicate_device (backend, added_device);
	if (!other_device)
		return TRUE;

	if (UP_IS_DEVICE_BLUEZ (added_device))
		bluez_device = added_device;
	else if (UP_IS_DEVICE_BLUEZ (other_device))
		bluez_device = other_device;

	if (bluez_device) {
		UpDevice *non_bluez_device;

		non_bluez_device = bluez_device == added_device ?
			other_device : added_device;
		g_object_bind_property (bluez_device, "model",
					non_bluez_device, "model",
					G_BINDING_SYNC_CREATE);
		unreg_device = bluez_device;
	} else {
		UpDeviceState state;
		UpDevice *tested_device;

		tested_device = added_device;
		g_object_get (G_OBJECT (tested_device), "state", &state, NULL);
		if (state != UP_DEVICE_STATE_UNKNOWN) {
			tested_device = other_device;
			g_object_get (G_OBJECT (tested_device), "state", &state, NULL);
		}
		if (state != UP_DEVICE_STATE_UNKNOWN) {
			g_object_get (G_OBJECT (added_device), "serial", &serial, NULL);
			g_debug ("Device %s is a duplicate, but we don't know if most interesting",
				 serial);
			return TRUE;
		}

		unreg_device = tested_device;
	}

	g_object_get (G_OBJECT (unreg_device), "serial", &serial, NULL);
	if (up_device_is_registered (unreg_device)) {
		g_signal_emit (backend, signals[SIGNAL_DEVICE_REMOVED], 0, unreg_device);
		up_device_unregister (unreg_device);
	}
	g_debug ("Hiding duplicate device %s", serial);
	return unreg_device != added_device;
}

static void
update_removed_duplicate_device (UpBackend *backend,
				 UpDevice  *removed_device)
{
	g_autoptr(UpDevice) other_device = NULL;

	other_device = find_duplicate_device (backend, removed_device);
	if (!other_device)
		return;

	/* Re-add the old duplicate device that got hidden */
	if (up_device_register (other_device)) {
		g_warning ("%s", G_STRFUNC);
		g_signal_emit (backend, signals[SIGNAL_DEVICE_ADDED], 0, other_device);
	}
}

static gboolean
is_interesting_iface_proxy (GDBusProxy *interface_proxy)
{
	const char *iface;

	iface = g_dbus_proxy_get_interface_name (interface_proxy);
	return g_str_equal (iface, "org.bluez.Battery1") ||
		g_str_equal (iface, "org.bluez.Device1");
}

static void gatt_debug(GDBusObject *object);
static void dump_properties(GDBusProxy *proxy);

static void
gatt_characteristic_debug(GDBusObject *object)
{
	GDBusObjectProxy *object_proxy;
	GDBusProxy *proxy;
	GError *error=NULL;
	const char *uuid;
	const char *service;
	GDBusInterface *iface;
	GDBusObjectProxy *service_proxy;

	g_debug("%s - %s", G_STRFUNC,  g_dbus_object_get_object_path (G_DBUS_OBJECT (object)));
	iface = g_dbus_object_get_interface (object, "org.bluez.GattCharacteristic1");
	if (iface == NULL) {
		g_debug("%s - no GATT characteristic interface for %s", G_STRFUNC, g_dbus_object_get_object_path (G_DBUS_OBJECT (object)));
		return;
	}
	//object_proxy = G_DBUS_OBJECT_PROXY (up_device_get_native (device));
	object_proxy = G_DBUS_OBJECT_PROXY (object);
	/* Initial battery values */
	proxy = g_dbus_proxy_new_sync (g_dbus_object_proxy_get_connection (object_proxy),
				       G_DBUS_PROXY_FLAGS_NONE,
				       NULL,
				       "org.bluez",
				       g_dbus_object_get_object_path (G_DBUS_OBJECT (object_proxy)),
				       "org.bluez.GattCharacteristic1",
				       NULL,
				       &error);

	if (!proxy) {
		g_warning ("Failed to get proxy for %s",
			   g_dbus_object_get_object_path (G_DBUS_OBJECT (object_proxy)));
		return;
	}

	dump_properties(proxy);

	uuid = g_variant_get_string (g_dbus_proxy_get_cached_property(proxy, "UUID"), NULL);
	g_warning("%s - %s - %s", G_STRFUNC, uuid, gatt_attribute_description(uuid));
	service = g_variant_get_string (g_dbus_proxy_get_cached_property(proxy, "Service"), NULL);
	g_warning("%s - %s", G_STRFUNC, service);
	service_proxy = g_dbus_object_proxy_new(g_dbus_object_proxy_get_connection(object_proxy), service);
	gatt_debug(G_DBUS_OBJECT(service_proxy));
	//percentage = g_variant_get_byte (g_dbus_proxy_get_cached_property (proxy, "Percentage"));
}

static void dump_uuids(GDBusObject *object)
{
	GDBusObjectProxy *object_proxy;
	GDBusProxy *proxy;
	GError *error=NULL;
	GDBusInterface *iface;

	g_debug("%s - %s", G_STRFUNC,  g_dbus_object_get_object_path (G_DBUS_OBJECT (object)));
	iface = g_dbus_object_get_interface (object, "org.bluez.Device1");
	if (iface == NULL) {
		g_debug("%s - no GATT characteristic interface for %s", G_STRFUNC, g_dbus_object_get_object_path (G_DBUS_OBJECT (object)));
		return;
	}
	//object_proxy = G_DBUS_OBJECT_PROXY (up_device_get_native (device));
	object_proxy = G_DBUS_OBJECT_PROXY (object);
	/* Initial battery values */
	proxy = g_dbus_proxy_new_sync (g_dbus_object_proxy_get_connection (object_proxy),
				       G_DBUS_PROXY_FLAGS_NONE,
				       NULL,
				       "org.bluez",
				       g_dbus_object_get_object_path (G_DBUS_OBJECT (object_proxy)),
				       "org.bluez.Device1",
				       NULL,
				       &error);

	if (!proxy) {
		g_warning ("Failed to get proxy for %s",
			   g_dbus_object_get_object_path (G_DBUS_OBJECT (object_proxy)));
		return;
	}

	const char **uuids = g_variant_get_strv (g_dbus_proxy_get_cached_property(proxy, "UUIDs"), NULL);
	for (const char **uuid = uuids; *uuid != NULL; uuid++) {
		g_warning("%s - %s - %s", G_STRFUNC, *uuid, gatt_attribute_description(*uuid));
	}
}

static void dump_properties(GDBusProxy *proxy)
{
	char **properties;
	properties = g_dbus_proxy_get_cached_property_names (proxy);
	if (properties == NULL) {
		g_warning ("no cached properties");
		return;
	} else {
		int i = 0;
		while (properties[i] != NULL) {
			g_warning ("%d: %s", i, properties[i]);
			i++;
		}
		g_strfreev(properties);
	}
}

static void dump_interfaces(GDBusObject *object)
{
	GList *interfaces;
	GList *it;
	g_debug("%s - %s", G_STRFUNC, g_dbus_object_get_object_path (G_DBUS_OBJECT (object)));
	interfaces = g_dbus_object_get_interfaces(object);
	for (it = interfaces; it != NULL; it = it -> next) {
		GDBusInterface *interface = G_DBUS_INTERFACE(it->data);
		g_debug("interface: %p", interface);
		GDBusInterfaceInfo *info = g_dbus_interface_get_info(interface);
		g_debug("info: %p", info);
		if (info != NULL) {
			g_debug("\tinterface: %s", info->name);
		}
		GDBusProxy *proxy = G_DBUS_PROXY(interface);
		info = g_dbus_proxy_get_interface_info(proxy);
		g_debug("proxy info: %p", info);
		if (info != NULL) {
			g_debug("\tinterface: %s", info->name);
		}
	}
	g_debug("finished dumping interfaces for %s", g_dbus_object_get_object_path (G_DBUS_OBJECT (object)));
}

static void
gatt_debug(GDBusObject *object)
{
	GDBusObjectProxy *object_proxy;
	GDBusProxy *proxy;
	GError *error=NULL;
	const char *uuid;
	GDBusInterface *iface;

	g_debug("%s - %s", G_STRFUNC,  g_dbus_object_get_object_path (G_DBUS_OBJECT (object)));
	iface = g_dbus_object_get_interface (object, "org.bluez.GattService1");
	if (iface == NULL) {
		g_debug("%s - no GATT service for %s", G_STRFUNC, g_dbus_object_get_object_path (G_DBUS_OBJECT (object)));
		dump_interfaces(object);
		return;
	}
	//object_proxy = G_DBUS_OBJECT_PROXY (up_device_get_native (device));
	object_proxy = G_DBUS_OBJECT_PROXY (object);
	/* Initial battery values */
	proxy = g_dbus_proxy_new_sync (g_dbus_object_proxy_get_connection (object_proxy),
				       G_DBUS_PROXY_FLAGS_NONE,
				       NULL,
				       "org.bluez",
				       g_dbus_object_get_object_path (G_DBUS_OBJECT (object_proxy)),
				       "org.bluez.GattService1",
				       NULL,
				       &error);

	if (!proxy) {
		g_warning ("Failed to get proxy for %s",
			   g_dbus_object_get_object_path (G_DBUS_OBJECT (object_proxy)));
		return;
	}

	dump_properties(proxy);

	uuid = g_variant_get_string (g_dbus_proxy_get_cached_property(proxy, "UUID"), NULL);
	g_warning("%s - %s - %s", G_STRFUNC, uuid, gatt_attribute_description(uuid));
	//percentage = g_variant_get_byte (g_dbus_proxy_get_cached_property (proxy, "Percentage"));
}

#define BATTERY_UUID "0000180f-0000-1000-8000-00805f9b34fb"
#define BATTERY_LEVEL_UUID "00002a19-0000-1000-8000-00805f9b34fb"
#define BATTERY_USER_DESC_UUID "00002901-0000-1000-8000-00805f9b34fb"

static GDBusProxy *bluez_new_proxy_for_iface(GDBusObject *object, const char *iface_name, GError **error)
{
	GDBusObjectProxy *object_proxy;
	g_autoptr(GDBusInterface) iface = NULL;

	iface = g_dbus_object_get_interface (object, iface_name);
	if (iface == NULL) {
		return NULL;
	}

	object_proxy = G_DBUS_OBJECT_PROXY (object);
	return g_dbus_proxy_new_sync (g_dbus_object_proxy_get_connection (object_proxy),
				       G_DBUS_PROXY_FLAGS_NONE,
				       NULL,
				       "org.bluez",
				       g_dbus_object_get_object_path (G_DBUS_OBJECT (object_proxy)),
				       iface_name,
				       NULL,
				       error);
}

static GDBusProxy *bluez_new_device_proxy(GDBusObject *object, GError **error)
{
	return bluez_new_proxy_for_iface (object, "org.bluez.Device1", error);
}


static GDBusProxy *bluez_new_service_proxy(GDBusObject *object, GError **error)
{
	return bluez_new_proxy_for_iface (object, "org.bluez.GattService1", error);
}

static GDBusProxy *bluez_new_characteristic_proxy(GDBusObject *object, GError **error)
{
	return bluez_new_proxy_for_iface (object, "org.bluez.GattCharacteristic1", error);
}

static GDBusProxy *bluez_new_descriptor_proxy(GDBusObject *object, GError **error)
{
	return bluez_new_proxy_for_iface (object, "org.bluez.GattDescriptor1", error);
}

static gboolean bluez_is_battery_service(GDBusProxy *proxy)
{
	g_autoptr(GVariant) prop_variant = NULL;

	prop_variant = g_dbus_proxy_get_cached_property(proxy, "UUID");
	if (prop_variant == NULL) {
		return FALSE;
	}
	const char *uuid = g_variant_get_string(prop_variant, NULL);

	return g_str_equal(uuid, BATTERY_UUID);
}

static gboolean bluez_gatt_uuid_equal(GDBusProxy *proxy, const char *wanted)
{
	g_autoptr(GVariant) prop_variant = NULL;

	prop_variant = g_dbus_proxy_get_cached_property(proxy, "UUID");
	if (prop_variant == NULL) {
		return FALSE;
	}
	const char *uuid = g_variant_get_string(prop_variant, NULL);

	return g_str_equal(uuid, wanted);
}
static gboolean bluez_characteristic_is_battery_level(GDBusProxy *proxy)
{
	return bluez_gatt_uuid_equal(proxy, BATTERY_LEVEL_UUID);
}

static gboolean bluez_descriptor_is_battery_user_desc(GDBusProxy *proxy)
{
	return bluez_gatt_uuid_equal(proxy, BATTERY_USER_DESC_UUID);
}

static int bluez_characteristic_read_battery_level(GDBusProxy *proxy)
{
	g_autoptr(GVariant) result = NULL;
	g_autoptr(GVariant) v = NULL;
	// need to call ReadValue({}) -> array of byte
	//GVariantBuilder builder;
	GVariant *empty_sv;

	empty_sv = g_variant_parse(G_VARIANT_TYPE("a{sv}"), "{}", NULL, NULL, NULL);
	//g_variant_builder_init(&builder, g_variant_type_new("a{sv}"));
	GVariant *parameters = g_variant_new_tuple(&empty_sv, 1);
	GError *error = NULL;
	result = g_dbus_proxy_call_sync (proxy,
					 "ReadValue",
					 parameters,
					 G_DBUS_CALL_FLAGS_NONE,
					 -1, NULL, &error);
	if (error != NULL) {
		g_debug("failed to call ReadValue: %s", error->message);
		return -1;
	}

	g_variant_get(result, "(@ay)", &v);
	gsize n_elems;
	const char *bytes = g_variant_get_fixed_array(v, &n_elems, sizeof(guint8));
	if (bytes == NULL) {
		return -1;
	}

	if ((n_elems < 1) || (n_elems > 1)) {
		g_debug("battery level should be a 8 bit value");
		return -1;
	}
	return bytes[0];
}

static GDBusObject *bluez_characteristic_get_device(UpBackend *backend, GDBusProxy *proxy)
{
	g_autoptr(GVariant) v = NULL;
	const char *service_path;

	v = g_dbus_proxy_get_cached_property(proxy, "Service");
	if (v == NULL) {
		return FALSE;
	}
	service_path = g_variant_get_string(v, NULL);
	if (service_path == NULL) {
		return NULL;
	}

	// gatt_battery_services maps from service object path to GDBusObject device
	return g_hash_table_lookup(backend->priv->gatt_battery_services, service_path);
}

static gboolean bluez_process_characteristic(UpBackend *backend, GDBusObject *object)
{
	g_autoptr(GDBusProxy) proxy = NULL;
	GError *error=NULL;
	GDBusObject *device;

	g_warning("%s - %s", G_STRFUNC,  g_dbus_object_get_object_path (G_DBUS_OBJECT (object)));
	proxy = bluez_new_characteristic_proxy(object, &error);
	if (!proxy) {
		g_warning ("Failed to get proxy for %s",
			   g_dbus_object_get_object_path (object));
		return FALSE;
	}

	if (bluez_characteristic_is_battery_level(proxy)) {
		device = bluez_characteristic_get_device(backend, proxy);
		if (device == NULL) {
			g_warning ("could not find service for characteristic %s", g_dbus_object_get_object_path (object));
			return FALSE;
		}
		UpDevice *up_device;
		up_device = UP_DEVICE (up_device_list_lookup_debug (backend->priv->device_list, G_OBJECT (device)));
		g_warning ("found device %s (%p)", g_dbus_object_get_object_path (device), up_device);


		int battery_level;
		battery_level = bluez_characteristic_read_battery_level(proxy);
		g_warning ("battery level for %s: %d%%", g_dbus_object_get_object_path (object), battery_level);
		return TRUE;
	}

	return FALSE;
}

static GDBusObject *bluez_service_get_device(UpBackend *backend, GDBusProxy *proxy)
{
	g_autoptr(GVariant) v = NULL;
	const char *device_path;

	v = g_dbus_proxy_get_cached_property(proxy, "Device");
	if (v == NULL) {
		return FALSE;
	}
	device_path = g_variant_get_string(v, NULL);
	if (device_path == NULL) {
		return NULL;
	}

	return g_hash_table_lookup(backend->priv->gatt_devices, device_path);
}

static gboolean bluez_process_service(UpBackend *backend, GDBusObject *object)
{
	g_autoptr(GDBusProxy) proxy = NULL;
	g_autoptr(GVariant) prop_variant = NULL;
	GError *error=NULL;

	g_warning("%s - %s", G_STRFUNC,  g_dbus_object_get_object_path (G_DBUS_OBJECT (object)));
	proxy = bluez_new_service_proxy(object, &error);
	if (!proxy) {
		g_warning ("Failed to get proxy for %s",
			   g_dbus_object_get_object_path (object));
		return FALSE;
	}

	GDBusObject *device;
	device = bluez_service_get_device(backend, proxy);
	if (device == NULL) {
		g_warning("found service %s before device, something is wrong", g_dbus_object_get_object_path(object));
		return FALSE;
	}
	// check if device was seen before, and has a battery service, otherwise something is wrong
	if (bluez_is_battery_service(proxy)) {
		if (device != NULL) {
			g_warning("%s: inserting service %s in hash table", G_STRFUNC, g_dbus_object_get_object_path(object));
			g_hash_table_insert(backend->priv->gatt_battery_services, (gpointer)g_dbus_object_get_object_path(object), g_object_ref(device));
			return TRUE;
		} else {
			g_warning ("should add service %s to hash table, but corresponding device is NULL", g_dbus_object_get_object_path(object));
		}
	}

	return FALSE;

}

static gboolean bluez_device_has_battery_service(GDBusProxy *proxy)
{
	g_autoptr(GVariant) prop_variant = NULL;
	const char **uuids;
	prop_variant = g_dbus_proxy_get_cached_property(proxy, "UUIDs");
	uuids = g_variant_get_strv (g_dbus_proxy_get_cached_property(proxy, "UUIDs"), NULL);
	for (const char **uuid = uuids; *uuid != NULL; uuid++) {
		if (g_str_equal(*uuid, BATTERY_UUID)) {
			return TRUE;
		}
	}

	return FALSE;
}

static gboolean
has_battery_iface (UpBackend *backend, GDBusObject *object)
{
	GDBusInterface *iface;

	//gatt_debug(object);
	iface = g_dbus_object_get_interface (object, "org.bluez.Battery1");
	g_debug ("%s: %s - %d", G_STRFUNC, g_dbus_object_get_object_path (object), iface != NULL);
	g_autoptr(GDBusProxy) device_proxy = NULL;
	GError *error = NULL;
	device_proxy = bluez_new_device_proxy(object, &error);
	//g_warning("%s: device_proxy: %p for %s %p", G_STRFUNC, device_proxy, g_dbus_object_get_object_path (object), error);
	//dump_interfaces(object);
	if (device_proxy != NULL) {
		if (bluez_device_has_battery_service(device_proxy)) {
			g_warning("%s: device %s has battery UUID", G_STRFUNC, g_dbus_object_get_object_path (object));
			g_hash_table_insert(backend->priv->gatt_devices, (gpointer)g_dbus_object_get_object_path (object), g_object_ref (object));
		} else {
			g_warning("%s: device %s does not have battery UUID", G_STRFUNC, g_dbus_object_get_object_path (object));
		}
	}
	/*
	if g_str_equal("/org/bluez/hci0/dev_FB_EC_57_5A_DB_B3", g_dbus_object_get_object_path (object)) {
		g_warning("dumping uuids for device");
		dump_uuids(object);
	}
	*/
	g_autoptr(GDBusProxy) service_proxy = NULL;
	service_proxy = bluez_new_service_proxy(object, NULL);
	if (service_proxy != NULL) {
		bluez_process_service (backend, object);
	}
	/*
	if ((service_proxy != NULL) && bluez_is_battery_service(service_proxy)) {
		g_warning("dumping interfaces for service %s", g_dbus_object_get_object_path (object));
		dump_interfaces(object);
		GDBusInterface *iface = g_dbus_object_get_interface(object, "org.freedesktop.DBus.Introspectable");
		g_warning("introspectable: %p", iface);
		iface = g_dbus_object_get_interface(object, "org.freedesktop.DBus.Properties");
		g_warning("properties: %p", iface);
		iface = g_dbus_object_get_interface(object, "org.bluez.GattService1");
		g_warning("gattservice: %p", iface);
		gatt_debug(object);
	}
	*/

	g_autoptr(GDBusProxy) characteristic_proxy = NULL;
	characteristic_proxy = bluez_new_characteristic_proxy(object, NULL);
	if (characteristic_proxy != NULL) {
		bluez_process_characteristic(backend, object);
	}
	g_autoptr(GDBusProxy) descriptor_proxy = NULL;
	descriptor_proxy = bluez_new_descriptor_proxy(object, NULL);
	if (descriptor_proxy != NULL) {
		//g_warning("%s - checking if descriptor is a user description", G_STRFUNC);
		if (bluez_descriptor_is_battery_user_desc(descriptor_proxy)) {
			g_warning ("bluez_process_descriptor - %s battery user desc", g_dbus_object_get_object_path (object));
		}
	}

	/*
	if ((characteristic_proxy != NULL) && bluez_is_battery_level_characteristic(characteristic_proxy)) {
		// right side: /org/bluez/hci0/dev_FB_EC_57_5A_DB_B3/service0015/char0016
		//  left side: /org/bluez/hci0/dev_FB_EC_57_5A_DB_B3/service0010/char0011
		g_warning("battery level right side! - %s", g_dbus_object_get_object_path (object));
		// implements GattCharacteristic1
		gatt_characteristic_debug(object);
	}
	*/
	//iface = g_dbus_object_get_interface (object, "org.bluez.Battery1");
	if (!iface)
		return FALSE;
	g_object_unref (iface);
	return TRUE;
}

static void
bluez_proxies_changed (GDBusObjectManagerClient *manager,
		       GDBusObjectProxy         *object_proxy,
		       GDBusProxy               *interface_proxy,
		       GVariant                 *changed_properties,
		       GStrv                     invalidated_properties,
		       gpointer                  user_data)
{
	UpBackend *backend = user_data;
	GObject *object;
	UpDeviceBluez *bluez;
	const char *iface_name;

	iface_name = g_dbus_proxy_get_interface_name (interface_proxy);
	g_warning("%s - %s - %s - %s", G_STRFUNC, g_dbus_object_get_object_path (G_DBUS_OBJECT (object_proxy)), iface_name, g_variant_print(changed_properties, FALSE));

	if (!is_interesting_iface_proxy (interface_proxy))
		return;

	object = up_device_list_lookup_debug (backend->priv->device_list, G_OBJECT (object_proxy));
	if (!object)
		return;

	bluez = UP_DEVICE_BLUEZ (object);
	g_warning("%s - %s", G_STRFUNC, up_device_get_object_path (UP_DEVICE (bluez)));
	up_device_bluez_update (bluez, changed_properties);
	g_object_unref (object);
}

static void
bluez_interface_removed (GDBusObjectManager *manager,
			 GDBusObject        *bus_object,
			 GDBusInterface     *interface,
			 gpointer            user_data)
{
	UpBackend *backend = user_data;
	GObject *object;

	g_debug("%s", G_STRFUNC);
	/* It might be another iface on another device that got removed */
	if (has_battery_iface (backend, bus_object))
		return;

	object = up_device_list_lookup (backend->priv->device_list, G_OBJECT (bus_object));
	if (!object)
		return;

	g_debug ("emitting device-removed: %s", g_dbus_object_get_object_path (bus_object));
	if (up_device_is_registered (UP_DEVICE (object)))
		g_signal_emit (backend, signals[SIGNAL_DEVICE_REMOVED], 0, UP_DEVICE (object));

	g_object_unref (object);
}

static void
bluez_interface_added (GDBusObjectManager *manager,
		       GDBusObject        *bus_object,
		       GDBusInterface     *interface,
		       gpointer            user_data)
{
	g_autoptr(UpDevice) device = NULL;
	UpBackend *backend = user_data;
	GObject *object;

	g_debug ("%s: %s", G_STRFUNC, g_dbus_object_get_object_path (bus_object));
	if (!has_battery_iface (backend, bus_object))
		return;


	object = up_device_list_lookup (backend->priv->device_list, G_OBJECT (bus_object));
	if (object != NULL) {
		g_object_unref (object);
		return;
	}

	device = g_initable_new (UP_TYPE_DEVICE_BLUEZ, NULL, NULL,
	                         "daemon", backend->priv->daemon,
	                         "native", G_OBJECT (bus_object),
	                         NULL);
	if (device) {
		g_debug ("emitting device-added: %s", g_dbus_object_get_object_path (bus_object));
		if (update_added_duplicate_device (backend, device)) {
			g_warning ("%s", G_STRFUNC);
			g_signal_emit (backend, signals[SIGNAL_DEVICE_ADDED], 0, device);
		}
	}
}

static void
bluez_appeared (GDBusConnection *connection,
		const gchar     *name,
		const gchar     *name_owner,
		gpointer         user_data)
{
	UpBackend *backend = user_data;
	GError *error = NULL;
	GList *objects, *l;

	g_assert (backend->priv->bluez_client == NULL);

	backend->priv->bluez_client = g_dbus_object_manager_client_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
										     G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_DO_NOT_AUTO_START,
										     "org.bluez",
										     "/",
										     NULL, NULL, NULL,
										     NULL, &error);
	if (!backend->priv->bluez_client) {
		g_warning ("Failed to create object manager for BlueZ: %s",
			   error->message);
		g_error_free (error);
		return;
	}

	g_debug ("BlueZ appeared");

	g_signal_connect (backend->priv->bluez_client, "interface-proxy-properties-changed",
			  G_CALLBACK (bluez_proxies_changed), backend);
	g_signal_connect (backend->priv->bluez_client, "interface-removed",
			  G_CALLBACK (bluez_interface_removed), backend);
	g_signal_connect (backend->priv->bluez_client, "interface-added",
			  G_CALLBACK (bluez_interface_added), backend);

	objects = g_dbus_object_manager_get_objects (backend->priv->bluez_client);
	for (l = objects; l != NULL; l = l->next) {
		GDBusObject *object = l->data;
		GList *interfaces, *k;

		interfaces = g_dbus_object_get_interfaces (object);

		for (k = interfaces; k != NULL; k = k->next) {
			GDBusInterface *iface = k->data;

			bluez_interface_added (backend->priv->bluez_client,
					       object,
					       iface,
					       backend);
			g_object_unref (iface);
		}
		g_list_free (interfaces);
		g_object_unref (object);
	}
	g_list_free (objects);
}

static void
bluez_vanished (GDBusConnection *connection,
		const gchar     *name,
		gpointer         user_data)
{
	UpBackend *backend = user_data;
	GPtrArray *array;
	guint i;

	g_debug ("BlueZ disappeared");

	array = up_device_list_get_array (backend->priv->device_list);

	for (i = 0; i < array->len; i++) {
		UpDevice *device = UP_DEVICE (g_ptr_array_index (array, i));
		if (UP_IS_DEVICE_BLUEZ (device)) {
			GDBusObject *object;

			object = G_DBUS_OBJECT (up_device_get_native (device));
			g_debug ("emitting device-removed: %s", g_dbus_object_get_object_path (object));
			if (up_device_is_registered (device))
				g_signal_emit (backend, signals[SIGNAL_DEVICE_REMOVED], 0, device);
		}
	}

	g_ptr_array_unref (array);

	g_clear_object (&backend->priv->bluez_client);
}

static void
up_device_disconnected_cb (GObject    *gobject,
			   GParamSpec *pspec,
			   gpointer    user_data)
{
	UpBackend *backend = user_data;
	g_autofree char *path = NULL;
	gboolean disconnected;

	g_object_get (gobject,
		      "native-path", &path,
		      "disconnected", &disconnected,
		      NULL);
	if (disconnected) {
		g_debug("Device %s became disconnected, hiding device", path);
		if (UP_IS_DEVICE (gobject)) {
			if (up_device_is_registered (UP_DEVICE (gobject))) {
				g_signal_emit (backend, signals[SIGNAL_DEVICE_REMOVED], 0, gobject);
				up_device_unregister (UP_DEVICE (gobject));
			}
		} else if (UP_IS_DEVICE_KBD_BACKLIGHT (gobject)) {
			up_device_kbd_backlight_unregister (UP_DEVICE_KBD_BACKLIGHT (gobject));
		}
	} else {
		g_debug ("Device %s became connected, showing device", path);
		if (up_device_register (UP_DEVICE (gobject))) {
			g_warning ("%s", G_STRFUNC);
			g_signal_emit (backend, signals[SIGNAL_DEVICE_ADDED], 0, gobject);
		}
	}
}

static void
udev_device_added_cb (UpBackend *backend, GObject *device)
{
	g_debug ("Got new device from udev enumerator: %p", device);
	g_signal_connect (device, "notify::disconnected",
			  G_CALLBACK (up_device_disconnected_cb), backend);
	if (UP_IS_DEVICE (device)) {
		if (update_added_duplicate_device (backend, UP_DEVICE (device))) {
			g_warning ("%s", G_STRFUNC);
			g_signal_emit (backend, signals[SIGNAL_DEVICE_ADDED], 0, device);
		}
	} else if (UP_IS_DEVICE_KBD_BACKLIGHT (device)) {
		g_warning ("%s #2", G_STRFUNC);
		g_signal_emit (backend, signals[SIGNAL_DEVICE_ADDED], 0, device);
	} else {
		g_warning ("Unknown device type");
	}
}

static void
udev_device_removed_cb (UpBackend *backend, GObject *device)
{
	g_debug ("Removing device from udev enumerator: %p", device);

	if (UP_IS_DEVICE (device))
		update_removed_duplicate_device (backend, UP_DEVICE (device));
	g_signal_emit (backend, signals[SIGNAL_DEVICE_REMOVED], 0, device);
}

/**
 * up_backend_coldplug:
 * @backend: The %UpBackend class instance
 * @daemon: The %UpDaemon controlling instance
 *
 * Finds all the devices already plugged in, and emits device-add signals for
 * each of them.
 *
 * Return value: %TRUE for success
 **/
gboolean
up_backend_coldplug (UpBackend *backend, UpDaemon *daemon)
{
	g_autolist(GUdevDevice) devices = NULL;
	GList *l;

	backend->priv->daemon = g_object_ref (daemon);
	backend->priv->device_list = up_daemon_get_device_list (daemon);

	/* Watch udev for input devices to find the lid switch */
	backend->priv->gudev_client = g_udev_client_new ((const char *[]){ "input", NULL });
	g_signal_connect (backend->priv->gudev_client, "uevent",
			  G_CALLBACK (up_backend_uevent_signal_handler_cb), backend);

	/* add all subsystems */
	devices = g_udev_client_query_by_subsystem (backend->priv->gudev_client, "input");
	for (l = devices; l != NULL; l = l->next)
		up_backend_uevent_signal_handler_cb (backend->priv->gudev_client,
						     "add",
						     G_UDEV_DEVICE (l->data),
						     backend);

	backend->priv->bluez_watch_id = g_bus_watch_name (G_BUS_TYPE_SYSTEM,
							  "org.bluez",
							  G_BUS_NAME_WATCHER_FLAGS_NONE,
							  bluez_appeared,
							  bluez_vanished,
							  backend,
							  NULL);

	backend->priv->udev_enum = g_object_new (UP_TYPE_ENUMERATOR_UDEV,
						 "daemon", daemon,
						 NULL);

	g_signal_connect_swapped (backend->priv->udev_enum, "device-added",
				  G_CALLBACK (udev_device_added_cb), backend);
	g_signal_connect_swapped (backend->priv->udev_enum, "device-removed",
				  G_CALLBACK (udev_device_removed_cb), backend);

	g_assert (g_initable_init (G_INITABLE (backend->priv->udev_enum), NULL, NULL));

	return TRUE;
}

/**
 * up_backend_unplug:
 * @backend: The %UpBackend class instance
 *
 * Forget about all learned devices, effectively undoing up_backend_coldplug.
 * Resources are released without emitting signals.
 */
void
up_backend_unplug (UpBackend *backend)
{
	g_clear_object (&backend->priv->gudev_client);
	g_clear_object (&backend->priv->udev_enum);
	g_clear_object (&backend->priv->device_list);
	g_clear_object (&backend->priv->lid_device);
	g_clear_object (&backend->priv->daemon);
	if (backend->priv->bluez_watch_id > 0) {
		g_bus_unwatch_name (backend->priv->bluez_watch_id);
		backend->priv->bluez_watch_id = 0;
	}
	g_clear_object (&backend->priv->bluez_client);
}

static gboolean
check_action_result (GVariant *result)
{
	if (result) {
		const char *s;

		g_variant_get (result, "(&s)", &s);
		if (g_strcmp0 (s, "yes") == 0)
			return TRUE;
	}
	return FALSE;
}

/**
 * up_backend_get_critical_action:
 * @backend: The %UpBackend class instance
 *
 * Which action will be taken when %UP_DEVICE_LEVEL_ACTION
 * warning-level occurs.
 **/
const char *
up_backend_get_critical_action (UpBackend *backend)
{
	struct {
		const gchar *method;
		const gchar *can_method;
	} actions[] = {
		{ "Suspend", "CanSuspend" },
		{ "HybridSleep", "CanHybridSleep" },
		{ "Hibernate", "CanHibernate" },
		{ "PowerOff", NULL },
		{ "Ignore", NULL },
	};
	g_autofree gchar *action = NULL;
	gboolean can_risky = FALSE;
	guint i = 1;

	g_return_val_if_fail (backend->priv->logind_proxy != NULL, NULL);

	can_risky = up_config_get_boolean (backend->priv->config,
					   "AllowRiskyCriticalPowerAction");

	/* find the configured action first */
	action = up_config_get_string (backend->priv->config, "CriticalPowerAction");

	/* safeguard for the risky actions */
	if (!can_risky) {
		if (!g_strcmp0 (action, "Suspend") || !g_strcmp0 (action, "Ignore")) {
			g_free (action);
			action = g_strdup_printf ("HybridSleep");
		}
	}

	if (action != NULL) {
		for (i = 0; i < G_N_ELEMENTS (actions); i++)
			if (g_str_equal (actions[i].method, action))
				break;
		if (i >= G_N_ELEMENTS (actions))
			i = 1;
	}

	for (; i < G_N_ELEMENTS (actions); i++) {
		GVariant *result;

		if (actions[i].can_method) {
			gboolean action_available;

			/* Check whether we can use the method */
			result = g_dbus_proxy_call_sync (backend->priv->logind_proxy,
							 actions[i].can_method,
							 NULL,
							 G_DBUS_CALL_FLAGS_NONE,
							 -1, NULL, NULL);
			action_available = check_action_result (result);
			g_variant_unref (result);

			if (!action_available)
				continue;
		}

		return actions[i].method;
	}
	g_assert_not_reached ();
}

/**
 * up_backend_take_action:
 * @backend: The %UpBackend class instance
 *
 * Act upon the %UP_DEVICE_LEVEL_ACTION warning-level.
 **/
void
up_backend_take_action (UpBackend *backend)
{
	const char *method;

	method = up_backend_get_critical_action (backend);
	g_assert (method != NULL);

	/* Take action */
	g_debug ("About to call logind method %s", method);

	/* Do nothing if the action is set to "Ignore" */
	if (g_strcmp0 (method, "Ignore") == 0) {
		return;
	}

	g_dbus_proxy_call (backend->priv->logind_proxy,
			   method,
			   g_variant_new ("(b)", FALSE),
			   G_DBUS_CALL_FLAGS_NONE,
			   G_MAXINT,
			   NULL,
			   NULL,
			   NULL);
}

/**
 * up_backend_inhibitor_lock_take:
 * @backend: The %UpBackend class instance
 * @reason: Why the inhibitor lock is taken
 * @mode: The mode of the lock ('delay' or 'block')
 *
 * Acquire a sleep inhibitor lock via systemd's logind that will
 * inhibit going to sleep until the lock is released again by
 * closing the file descriptor.
 */
int
up_backend_inhibitor_lock_take (UpBackend  *backend,
                                const char *reason,
                                const char *mode)
{
	GVariant *out, *input;
	GUnixFDList *fds = NULL;
	int fd;
	GError *error = NULL;

	g_return_val_if_fail (reason != NULL, -1);
	g_return_val_if_fail (mode != NULL, -1);
	g_return_val_if_fail (g_str_equal (mode, "delay") || g_str_equal (mode, "block"), -1);

	input = g_variant_new ("(ssss)",
			       "sleep",  /* what */
			       "UPower", /* who */
			       reason,   /* why */
			       mode);    /* mode */

	out = g_dbus_proxy_call_with_unix_fd_list_sync (backend->priv->logind_proxy,
							"Inhibit",
							input,
							G_DBUS_CALL_FLAGS_NONE,
							-1,
							NULL,
							&fds,
							NULL,
							&error);
	if (out == NULL) {
		g_warning ("Could not acquire inhibitor lock: %s",
			   error ? error->message : "Unknown reason");
		g_clear_error (&error);
		return -1;
	}

	if (g_unix_fd_list_get_length (fds) != 1) {
		g_warning ("Unexpected values returned by logind's 'Inhibit'");
		g_variant_unref (out);
		g_object_unref (fds);
		return -1;
	}

	fd = g_unix_fd_list_get (fds, 0, NULL);

	g_variant_unref (out);
	g_object_unref (fds);

	g_debug ("Acquired inhibitor lock (%i, %s)", fd, mode);

	return fd;
}

/**
 * up_backend_prepare_for_sleep:
 *
 * Callback for logind's PrepareForSleep signal. It receives
 * a boolean that indicates if we are about to sleep (TRUE)
 * or waking up (FALSE).
 * In case of the waking up we refresh the devices so we are
 * up to date, especially w.r.t. battery levels, since they
 * might have changed drastically.
 **/
static void
up_backend_prepare_for_sleep (GDBusConnection *connection,
			      const gchar     *sender_name,
			      const gchar     *object_path,
			      const gchar     *interface_name,
			      const gchar     *signal_name,
			      GVariant        *parameters,
			      gpointer         user_data)
{
	UpBackend *backend = user_data;
	gboolean will_sleep;
	GPtrArray *array;
	guint i;

	if (!g_variant_is_of_type (parameters, G_VARIANT_TYPE ("(b)"))) {
		g_warning ("logind PrepareForSleep has unexpected parameter(s)");
		return;
	}

	g_variant_get (parameters, "(b)", &will_sleep);

	if (will_sleep) {
		up_daemon_pause_poll (backend->priv->daemon);
		if (backend->priv->logind_delay_inhibitor_fd >= 0) {
			close (backend->priv->logind_delay_inhibitor_fd);
			backend->priv->logind_delay_inhibitor_fd = -1;
		}
		return;
	}

	if (backend->priv->logind_delay_inhibitor_fd < 0)
		backend->priv->logind_delay_inhibitor_fd = up_backend_inhibitor_lock_take (backend, "Pause device polling", "delay");

	/* we are waking up, lets refresh all battery devices */
	g_debug ("Woke up from sleep; about to refresh devices");
	array = up_device_list_get_array (backend->priv->device_list);

	for (i = 0; i < array->len; i++) {
		UpDevice *device = UP_DEVICE (g_ptr_array_index (array, i));
		up_device_refresh_internal (device, UP_REFRESH_RESUME);
	}

	g_ptr_array_unref (array);

	up_daemon_resume_poll (backend->priv->daemon);
}


static void
up_backend_class_init (UpBackendClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	object_class->finalize = up_backend_finalize;

	signals [SIGNAL_DEVICE_ADDED] =
		g_signal_new ("device-added",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (UpBackendClass, device_added),
			      NULL, NULL, NULL,
			      G_TYPE_NONE, 1, G_TYPE_OBJECT);
	signals [SIGNAL_DEVICE_REMOVED] =
		g_signal_new ("device-removed",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (UpBackendClass, device_removed),
			      NULL, NULL, NULL,
			      G_TYPE_NONE, 1, G_TYPE_OBJECT);
}

static void
up_backend_init (UpBackend *backend)
{
	GDBusConnection *bus;
	guint sleep_id;

	backend->priv = up_backend_get_instance_private (backend);
	backend->priv->config = up_config_new ();
	backend->priv->logind_proxy = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
								     0,
								     NULL,
								     LOGIND_DBUS_NAME,
								     LOGIND_DBUS_PATH,
								     LOGIND_DBUS_INTERFACE,
								     NULL,
								     NULL);

	bus = g_dbus_proxy_get_connection (backend->priv->logind_proxy);
	sleep_id = g_dbus_connection_signal_subscribe (bus,
						       LOGIND_DBUS_NAME,
						       LOGIND_DBUS_INTERFACE,
						       "PrepareForSleep",
						       LOGIND_DBUS_PATH,
						       NULL,
						       G_DBUS_SIGNAL_FLAGS_NONE,
						       up_backend_prepare_for_sleep,
						       backend,
						       NULL);
	backend->priv->logind_sleep_id = sleep_id;
	backend->priv->logind_delay_inhibitor_fd = -1;

	backend->priv->logind_delay_inhibitor_fd = up_backend_inhibitor_lock_take (backend, "Pause device polling", "delay");

	backend->priv->gatt_devices = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, g_object_unref);
	backend->priv->gatt_battery_services = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, g_object_unref);
}

static void
up_backend_finalize (GObject *object)
{
	UpBackend *backend;
	GDBusConnection *bus;

	g_return_if_fail (UP_IS_BACKEND (object));

	backend = UP_BACKEND (object);

	if (backend->priv->bluez_watch_id > 0) {
		g_bus_unwatch_name (backend->priv->bluez_watch_id);
		backend->priv->bluez_watch_id = 0;
	}
	g_clear_object (&backend->priv->bluez_client);
	g_clear_pointer (&backend->priv->gatt_battery_services, g_hash_table_destroy);
	g_clear_pointer (&backend->priv->gatt_devices, g_hash_table_destroy);

	g_clear_object (&backend->priv->config);
	g_clear_object (&backend->priv->daemon);
	g_clear_object (&backend->priv->device_list);
	g_clear_object (&backend->priv->gudev_client);

	bus = g_dbus_proxy_get_connection (backend->priv->logind_proxy);
	g_dbus_connection_signal_unsubscribe (bus,
					      backend->priv->logind_sleep_id);

	if (backend->priv->logind_delay_inhibitor_fd >= 0)
		close (backend->priv->logind_delay_inhibitor_fd);

	g_clear_object (&backend->priv->logind_proxy);

	g_clear_object (&backend->priv->lid_device);

	G_OBJECT_CLASS (up_backend_parent_class)->finalize (object);
}

/**
 * up_backend_new:
 *
 * Return value: a new %UpBackend object.
 **/
UpBackend *
up_backend_new (void)
{
	return g_object_new (UP_TYPE_BACKEND, NULL);
}
