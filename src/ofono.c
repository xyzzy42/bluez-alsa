/*
 * BlueALSA - ofono.c
 * Copyright (c) 2016-2019 Arkadiusz Bokowy
 *               2018 Thierry Bultel
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 * When oFono is running on a system, it registers itself to BlueZ as an HFP
 * profile, which conflicts with our internal "--hfp-ag" and "--hpf-hf" ones.
 * This file is an implementation of the oFono back-end for bluez-alsa.
 *
 * For more details, see:
 * https://github.com/rilmodem/ofono/blob/master/doc/handsfree-audio-api.txt
 *
 */

#include "ofono.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <glib-object.h>
#include <glib.h>

#include "ba-adapter.h"
#include "ba-device.h"
#include "ba-transport.h"
#include "bluealsa.h"
#include "bluealsa-dbus.h"
#include "hfp.h"
#include "ofono-iface.h"
#include "shared/log.h"

/**
 * Lookup data associated with oFono card. */
struct ofono_card_data {
	int hci_dev_id;
	bdaddr_t bt_addr;
	char transport_path[32];
};

static GHashTable *ofono_card_data_map = NULL;
static unsigned int dbus_agent_object_id = 0;

/**
 * Authorize oFono SCO connection.
 *
 * @param fd SCO socket file descriptor.
 * @return On success this function returns 0. Otherwise, -1 is returned and
 *	 errno is set to indicate the error. */
static int ofono_sco_socket_authorize(int fd) {

	struct pollfd pfd = { fd, POLLOUT, 0 };
	char c;

	if (poll(&pfd, 1, 0) == -1)
		return -1;

	/* If socket is not writable, it means that it is in the defer setup
	 * state, so it needs to be read to authorize the connection. */
	if (!(pfd.revents & POLLOUT) && read(fd, &c, 1) == -1)
		return -1;

	return 0;
}

/**
 * Ask oFono to connect to a card (in return it will call NewConnection). */
static int ofono_acquire_bt_sco(struct ba_transport *t) {

	GDBusConnection *conn = config.dbus;
	GDBusMessage *msg = NULL, *rep = NULL;
	GError *err = NULL;
	int ret = 0;

	debug("Requesting new oFono SCO connection: %s", t->bluez_dbus_path);

	const char *ofono_dbus_path = &t->bluez_dbus_path[6];
	msg = g_dbus_message_new_method_call(t->bluez_dbus_owner, ofono_dbus_path,
			OFONO_IFACE_HF_AUDIO_CARD, "Connect");

	if ((rep = g_dbus_connection_send_message_with_reply_sync(conn, msg,
					G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL, &err)) == NULL)
		goto fail;

	if (g_dbus_message_get_message_type(rep) == G_DBUS_MESSAGE_TYPE_ERROR) {
		g_dbus_message_to_gerror(rep, &err);
		goto fail;
	}

	goto final;

fail:
	ret = -1;

final:
	if (msg != NULL)
		g_object_unref(msg);
	if (rep != NULL)
		g_object_unref(rep);
	if (err != NULL) {
		warn("Couldn't connect to card: %s", err->message);
		g_error_free(err);
	}

	return ret;
}

/**
 * Disconnects from a card.
 *
 * This is basically called when no PCM client is connected, in order to save
 * bandwidth for Bluetooth.
 *
 * @return On success this function returns 0. Otherwise -1 is returned. */
static int ofono_release_bt_sco(struct ba_transport *t) {

	if (t->bt_fd == -1)
		return 0;

	debug("Closing oFono SCO: %d", t->bt_fd);

	shutdown(t->bt_fd, SHUT_RDWR);
	close(t->bt_fd);

	t->bt_fd = -1;
	t->type.codec = HFP_CODEC_UNDEFINED;

	bluealsa_dbus_transport_update(t,
			BA_DBUS_TRANSPORT_UPDATE_SAMPLING | BA_DBUS_TRANSPORT_UPDATE_CODEC);

	return 0;
}

/**
 * Create new oFono transport.
 *
 * It will be created with an unset codec, which is the condition for it
 * to be hidden to clients. The codec will be set when the phone call starts.
 *
 * @return On success, the pointer to the newly allocated transport structure
 *   is returned. If error occurs, NULL is returned and the errno variable is
 *   set to indicated the cause of the error. */
static struct ba_transport *ofono_transport_new(
		struct ba_device *device,
		struct ba_transport_type type,
		const char *dbus_owner,
		const char *dbus_path) {

	struct ba_transport *t;

	if ((t = ba_transport_new_sco(device, type, dbus_owner, dbus_path, NULL)) == NULL)
		return NULL;

	t->sco.ofono = true;
	t->acquire = ofono_acquire_bt_sco;
	t->release = ofono_release_bt_sco;

	return t;
}

/**
 * Add new oFono card (phone). */
static void ofono_card_add(const char *dbus_sender, const char *card,
		GVariantIter *properties) {

	struct ba_adapter *a = NULL;
	struct ba_device *d = NULL;
	struct ba_transport *t = NULL;

	const char *key = NULL;
	GVariant *value = NULL;
	bdaddr_t addr_dev = { 0 };
	bdaddr_t addr_hci = { 0 };
	int hci_dev_id = -1;

	struct ba_transport_type ttype = {
		.profile = BA_TRANSPORT_PROFILE_HFP_HF,
		.codec = HFP_CODEC_UNDEFINED,
	};

	while (g_variant_iter_next(properties, "{&sv}", &key, &value)) {
		if (strcmp(key, "RemoteAddress") == 0)
			str2ba(g_variant_get_string(value, NULL), &addr_dev);
		else if (strcmp(key, "LocalAddress") == 0) {
			str2ba(g_variant_get_string(value, NULL), &addr_hci);
			hci_dev_id = hci_get_route(&addr_hci);
		}
		else if (strcmp(key, "Type") == 0) {
			const char *type = g_variant_get_string(value, NULL);
			if (strcmp(type, OFONO_AUDIO_CARD_TYPE_AG) == 0)
				ttype.profile = BA_TRANSPORT_PROFILE_HFP_AG;
			else if (strcmp(type, OFONO_AUDIO_CARD_TYPE_HF) == 0)
				ttype.profile = BA_TRANSPORT_PROFILE_HFP_HF;
			else {
				error("Unsupported profile type: %s", type);
				goto fail;
			}
		}
		g_variant_unref(value);
		value = NULL;
	}

	debug("Adding new oFono card: %s", card);

	if ((a = ba_adapter_lookup(hci_dev_id)) == NULL &&
			(a = ba_adapter_new(hci_dev_id, NULL)) == NULL) {
		error("Couldn't create new adapter: %s", strerror(errno));
		goto fail;
	}

	pthread_mutex_lock(&a->devices_mutex);

	if ((d = ba_device_lookup(a, &addr_dev)) == NULL &&
			(d = ba_device_new(a, &addr_dev)) == NULL) {
		error("Couldn't create new device: %s", strerror(errno));
		goto fail;
	}

	struct ofono_card_data ocd = {
		.hci_dev_id = hci_dev_id,
		.bt_addr = addr_dev,
	};

	snprintf(ocd.transport_path, sizeof(ocd.transport_path), "/ofono%s", card);
	if ((t = ofono_transport_new(d, ttype, dbus_sender, ocd.transport_path)) == NULL) {
		error("Couldn't create new transport: %s", strerror(errno));
		goto fail;
	}

	g_hash_table_insert(ofono_card_data_map, g_strdup(card),
			g_memdup(&ocd, sizeof(ocd)));

	ba_transport_set_state(t, TRANSPORT_ACTIVE);

fail:
	if (a != NULL)
		pthread_mutex_unlock(&a->devices_mutex);
	if (value != NULL)
		g_variant_unref(value);
}

/**
 * Get all oFono cards (phones). */
static int ofono_get_all_cards(void) {

	GDBusConnection *conn = config.dbus;
	GDBusMessage *msg = NULL, *rep = NULL;
	GError *err = NULL;
	int ret = 0;

	msg = g_dbus_message_new_method_call(OFONO_SERVICE, "/",
			OFONO_IFACE_HF_AUDIO_MANAGER, "GetCards");

	if ((rep = g_dbus_connection_send_message_with_reply_sync(conn, msg,
					G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL, &err)) == NULL)
		goto fail;

	if (g_dbus_message_get_message_type(rep) == G_DBUS_MESSAGE_TYPE_ERROR) {
		g_dbus_message_to_gerror(rep, &err);
		goto fail;
	}

	const char *sender = g_dbus_message_get_sender(rep);
	GVariant *body = g_dbus_message_get_body(rep);

	GVariantIter *cards;
	GVariantIter *properties = NULL;
	const char *card;

	g_variant_get(body, "(a(oa{sv}))", &cards);
	while (g_variant_iter_next(cards, "(&oa{sv})", &card, &properties))
		ofono_card_add(sender, card, properties);

	goto final;

fail:
	ret = -1;

final:
	if (msg != NULL)
		g_object_unref(msg);
	if (rep != NULL)
		g_object_unref(rep);
	if (err != NULL) {
		warn("Couldn't get oFono cards: %s", err->message);
		g_error_free(err);
	}

	return ret;
}

/**
 * Remove all oFono cards and deletes associated transports. */
static void ofono_remove_all_cards(void) {

	GHashTableIter iter;
	struct ofono_card_data *ocd;

	g_hash_table_iter_init(&iter, ofono_card_data_map);
	while (g_hash_table_iter_next(&iter, NULL, (gpointer)&ocd)) {

		struct ba_adapter *a;
		struct ba_device *d;
		struct ba_transport *t;

		if ((a = ba_adapter_lookup(ocd->hci_dev_id)) == NULL)
			goto fail;
		pthread_mutex_lock(&a->devices_mutex);
		if ((d = ba_device_lookup(a, &ocd->bt_addr)) == NULL)
			goto fail;
		if ((t = ba_transport_lookup(d, ocd->transport_path)) == NULL)
			goto fail;

		ba_transport_free(t);

fail:
		if (a != NULL)
			pthread_mutex_unlock(&a->devices_mutex);
	}

}

static void ofono_agent_new_connection(GDBusMethodInvocation *inv, void *userdata) {
	(void)userdata;

	GDBusMessage *msg = g_dbus_method_invocation_get_message(inv);
	GVariant *params = g_dbus_method_invocation_get_parameters(inv);

	struct ba_adapter *a = NULL;
	struct ba_device *d = NULL;
	struct ba_transport *t = NULL;

	GError *err = NULL;
	GUnixFDList *fd_list;
	const char *card;
	uint8_t codec;
	int fd;

	g_variant_get(params, "(&ohy)", &card, &fd, &codec);

	fd_list = g_dbus_message_get_unix_fd_list(msg);
	if ((fd = g_unix_fd_list_get(fd_list, 0, &err)) == -1) {
		error("Couldn't obtain SCO socket: %s", err->message);
		goto fail;
	}

	struct ofono_card_data *ocd;
	if ((ocd = g_hash_table_lookup(ofono_card_data_map, card)) == NULL) {
		error("Card data not available: %s", card);
		goto fail;
	}

	if ((a = ba_adapter_lookup(ocd->hci_dev_id)) == NULL)
		goto fail;
	pthread_mutex_lock(&a->devices_mutex);
	if ((d = ba_device_lookup(a, &ocd->bt_addr)) == NULL)
		goto fail;
	if ((t = ba_transport_lookup(d, ocd->transport_path)) == NULL)
		goto fail;

	if (ofono_sco_socket_authorize(fd) == -1) {
		error("Couldn't authorize SCO connection: %s", strerror(errno));
		goto fail;
	}

	debug("New oFono SCO connection (codec: %#x): %d", codec, fd);

	t->bt_fd = fd;
	t->type.codec = codec;

	t->mtu_read = 48;
	t->mtu_write = 48;

	bluealsa_dbus_transport_update(t,
			BA_DBUS_TRANSPORT_UPDATE_SAMPLING | BA_DBUS_TRANSPORT_UPDATE_CODEC);

	ba_transport_send_signal(t, TRANSPORT_PING);

	g_dbus_method_invocation_return_value(inv, NULL);
	goto final;

fail:
	g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
		G_DBUS_ERROR_INVALID_ARGS, "Unable to get connection");
	if (fd != -1)
		close(fd);

final:
	if (a != NULL)
		pthread_mutex_unlock(&a->devices_mutex);
	if (err != NULL)
		g_error_free(err);
}

/**
 * Callback for the Release method, called when oFono is properly shutdown. */
static void ofono_agent_release(GDBusMethodInvocation *inv, void *userdata) {
	(void)userdata;

	GDBusConnection *conn = g_dbus_method_invocation_get_connection(inv);

	g_dbus_connection_unregister_object(conn, dbus_agent_object_id);
	ofono_remove_all_cards();

	g_object_unref(inv);
}

static void ofono_hf_audio_agent_method_call(GDBusConnection *conn, const char *sender,
		const char *path, const char *interface, const char *method, GVariant *params,
		GDBusMethodInvocation *invocation, void *userdata) {
	debug("Called: %s.%s()", interface, method);
	(void)conn;
	(void)sender;
	(void)path;
	(void)params;

	if (strcmp(method, "NewConnection") == 0)
		ofono_agent_new_connection(invocation, userdata);
	else if (strcmp(method, "Release") == 0)
		ofono_agent_release(invocation, userdata);

}

/**
 * Register to the oFono service.
 *
 * @return On success this function returns 0. Otherwise -1 is returned. */
int ofono_register(void) {

	static GDBusInterfaceVTable vtable = {
		.method_call = ofono_hf_audio_agent_method_call,
	};

	const char *path = "/org/bluez/HFP/oFono";
	GDBusConnection *conn = config.dbus;
	GDBusMessage *msg = NULL, *rep = NULL;
	GError *err = NULL;
	int ret = 0;

	if (!config.enable.hfp_ofono)
		goto final;

	if (ofono_card_data_map == NULL)
		ofono_card_data_map = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

	debug("Registering oFono audio agent: %s", path);
	if ((dbus_agent_object_id = g_dbus_connection_register_object(conn, path,
					(GDBusInterfaceInfo *)&ofono_iface_hf_audio_agent, &vtable,
					NULL, NULL, &err)) == 0)
		goto fail;

	msg = g_dbus_message_new_method_call(OFONO_SERVICE, "/",
			OFONO_IFACE_HF_AUDIO_MANAGER, "Register");

	GVariantBuilder options;

	g_variant_builder_init(&options, G_VARIANT_TYPE("ay"));
	g_variant_builder_add(&options, "y", OFONO_AUDIO_CODEC_CVSD);

	g_dbus_message_set_body(msg, g_variant_new("(oay)", path, &options));
	g_variant_builder_clear(&options);

	if ((rep = g_dbus_connection_send_message_with_reply_sync(conn, msg,
					G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL, &err)) == NULL)
		goto fail;

	if (g_dbus_message_get_message_type(rep) == G_DBUS_MESSAGE_TYPE_ERROR) {
		g_dbus_message_to_gerror(rep, &err);
		goto fail;
	}

	ofono_get_all_cards();

	goto final;

fail:
	ret = -1;

final:
	if (msg != NULL)
		g_object_unref(msg);
	if (rep != NULL)
		g_object_unref(rep);
	if (err != NULL) {
		warn("Couldn't register oFono: %s", err->message);
		g_dbus_connection_unregister_object(conn, dbus_agent_object_id);
		g_error_free(err);
	}

	return ret;
}

/**
 * Callback for the CardAdded signal (emitted when phone is connected). */
static void ofono_signal_card_added(GDBusConnection *conn, const char *sender,
		const char *path, const char *interface, const char *signal, GVariant *params,
		void *userdata) {
	debug("Signal: %s.%s()", interface, signal);
	(void)conn;
	(void)path;
	(void)userdata;

	const char *card = NULL;
	GVariantIter *properties = NULL;

	g_variant_get(params, "(&oa{sv})", &card, &properties);
	ofono_card_add(sender, card, properties);

	g_variant_iter_free(properties);
}

/**
 * Callback for the CardRemoved signal (emitted when phone is disconnected). */
static void ofono_signal_card_removed(GDBusConnection *conn, const char *sender,
		const char *path, const char *interface, const char *signal, GVariant *params,
		void *userdata) {
	debug("Signal: %s.%s()", interface, signal);
	(void)conn;
	(void)sender;
	(void)path;
	(void)userdata;

	struct ba_adapter *a = NULL;
	struct ba_device *d = NULL;
	struct ba_transport *t = NULL;

	const char *card = NULL;
	g_variant_get(params, "(&o)", &card);

	struct ofono_card_data *ocd;
	if ((ocd = g_hash_table_lookup(ofono_card_data_map, card)) == NULL) {
		error("Card data not available: %s", card);
		goto fail;
	}

	if ((a = ba_adapter_lookup(ocd->hci_dev_id)) == NULL)
		goto fail;
	pthread_mutex_lock(&a->devices_mutex);
	if ((d = ba_device_lookup(a, &ocd->bt_addr)) == NULL)
		goto fail;
	if ((t = ba_transport_lookup(d, ocd->transport_path)) == NULL)
		goto fail;

	debug("Removing oFono card: %s", card);

	ba_transport_free(t);

fail:
	if (a != NULL)
		pthread_mutex_unlock(&a->devices_mutex);
}

/**
 * Monitor oFono service availability.
 *
 * When oFono is properly shutdown, we are notified through the Release()
 * method. Here, we get the opportunity to perform some cleanup if oFono
 * was killed. */
static void ofono_signal_name_owner_changed(GDBusConnection *conn, const char *sender,
		const char *path, const char *interface, const char *signal, GVariant *params,
		void *userdata) {
	(void)conn;
	(void)sender;
	(void)path;
	(void)interface;
	(void)signal;
	(void)userdata;

	const char *name;
	const char *owner_old;
	const char *owner_new;

	g_variant_get(params, "(&s&s&s)", &name, &owner_old, &owner_new);

	if (owner_old != NULL && owner_old[0] != '\0') {
		g_dbus_connection_unregister_object(conn, dbus_agent_object_id);
		ofono_remove_all_cards();
	}
	if (owner_new != NULL && owner_new[0] != '\0')
		ofono_register();

}

/**
 * Subscribe to oFono signals.
 *
 * @return On success this function returns 0. Otherwise -1 is returned. */
int ofono_subscribe_signals(void) {

	if (!config.enable.hfp_ofono)
		return 0;

	g_dbus_connection_signal_subscribe(config.dbus, OFONO_SERVICE,
			OFONO_IFACE_HF_AUDIO_MANAGER, "CardAdded", NULL, NULL,
			G_DBUS_SIGNAL_FLAGS_NONE, ofono_signal_card_added, NULL, NULL);
	g_dbus_connection_signal_subscribe(config.dbus, OFONO_SERVICE,
			OFONO_IFACE_HF_AUDIO_MANAGER, "CardRemoved", NULL, NULL,
			G_DBUS_SIGNAL_FLAGS_NONE, ofono_signal_card_removed, NULL, NULL);

	g_dbus_connection_signal_subscribe(config.dbus, "org.freedesktop.DBus",
			"org.freedesktop.DBus", "NameOwnerChanged", NULL, OFONO_SERVICE,
			G_DBUS_SIGNAL_FLAGS_NONE, ofono_signal_name_owner_changed, NULL, NULL);

	return 0;
}
