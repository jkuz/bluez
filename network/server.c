/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2004-2008  Marcel Holtmann <marcel@holtmann.org>
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <net/if.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <bluetooth/bnep.h>
#include <bluetooth/l2cap.h>
#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>

#include <netinet/in.h>

#include <glib.h>

#include "logging.h"
#include "dbus.h"
#include "error.h"
#include "textfile.h"
#include "dbus-helper.h"
#include "sdpd.h"

#define NETWORK_SERVER_INTERFACE "org.bluez.network.Server"
#define SETUP_TIMEOUT		1000
#define MAX_SETUP_ATTEMPTS	3

#include "bridge.h"
#include "common.h"
#include "manager.h"

/* Pending Authorization */
struct setup_session {
	char		*address;	/* Remote Bluetooth Address */
	uint16_t	dst_role;	/* Destination role */
	uint16_t	src_role;	/* Source role */
	int		nsk;		/* L2CAP socket */
	int		attempts;	/* Setup msg received */
	guint		watch;		/* BNEP setup watch */
	guint		timeout;	/* Max setup time */
};

/* Main server structure */
struct network_server {
	bdaddr_t	src;		/* Bluetooth Local Address */
	char		*iface;		/* Routing interface */
	char		*name;		/* Server service name */
	char		*range;		/* IP Address range */
	char		*path;		/* D-Bus path */
	gboolean	enable;		/* Enable flag */
	uint32_t	record_id;	/* Service record id */
	uint16_t	id;		/* Service class identifier */
	GSList		*clients;	/* Active connections */
};

static GIOChannel *bnep_io = NULL;
static DBusConnection *connection = NULL;
static GSList *setup_sessions = NULL;
static const char *prefix = NULL;
static gboolean security = TRUE;

static int store_property(bdaddr_t *src, uint16_t id,
			const char *key, const char *value)
{
	char filename[PATH_MAX + 1];
	char addr[18];

	ba2str(src, addr);
	if (id == BNEP_SVC_NAP)
		create_name(filename, PATH_MAX, STORAGEDIR, addr, "nap");
	else if (id == BNEP_SVC_GN)
		create_name(filename, PATH_MAX, STORAGEDIR, addr, "gn");
	else if (id == BNEP_SVC_PANU)
		create_name(filename, PATH_MAX, STORAGEDIR, addr, "panu");

	return textfile_put(filename, key, value);
}

static void setup_free(struct setup_session *s)
{
	g_free(s->address);
	g_free(s);
}

static int setup_cmp(const struct setup_session *s, const char *addr)
{
	return strcmp(s->address, addr);
}

static void add_lang_attr(sdp_record_t *r)
{
	sdp_lang_attr_t base_lang;
	sdp_list_t *langs = 0;

	/* UTF-8 MIBenum (http://www.iana.org/assignments/character-sets) */
	base_lang.code_ISO639 = (0x65 << 8) | 0x6e;
	base_lang.encoding = 106;
	base_lang.base_offset = SDP_PRIMARY_LANG_BASE;
	langs = sdp_list_append(0, &base_lang);
	sdp_set_lang_attr(r, langs);
	sdp_list_free(langs, 0);
}

sdp_record_t *server_record_new(const char *name, uint16_t id)
{
	sdp_list_t *svclass, *pfseq, *apseq, *root, *aproto;
	uuid_t root_uuid, pan, l2cap, bnep;
	sdp_profile_desc_t profile[1];
	sdp_list_t *proto[2];
	sdp_data_t *v, *p;
	uint16_t psm = BNEP_PSM, version = 0x0100;
	uint16_t security_desc = (security ? 0x0001 : 0x0000);
	uint16_t net_access_type = 0xfffe;
	uint32_t max_net_access_rate = 0;
	const char *desc = "BlueZ PAN service";
	sdp_record_t *record;

	record = sdp_record_alloc();
	if (!record)
		return NULL;

	record->attrlist = NULL;
	record->pattern = NULL;

	switch (id) {
	case BNEP_SVC_NAP:
		sdp_uuid16_create(&pan, NAP_SVCLASS_ID);
		svclass = sdp_list_append(NULL, &pan);
		sdp_set_service_classes(record, svclass);

		sdp_uuid16_create(&profile[0].uuid, NAP_PROFILE_ID);
		profile[0].version = 0x0100;
		pfseq = sdp_list_append(NULL, &profile[0]);
		sdp_set_profile_descs(record, pfseq);

		sdp_set_info_attr(record, name, NULL, desc);

		sdp_attr_add_new(record, SDP_ATTR_NET_ACCESS_TYPE,
					SDP_UINT16, &net_access_type);
		sdp_attr_add_new(record, SDP_ATTR_MAX_NET_ACCESSRATE,
					SDP_UINT32, &max_net_access_rate);
		break;
	case BNEP_SVC_GN:
		sdp_uuid16_create(&pan, GN_SVCLASS_ID);
		svclass = sdp_list_append(NULL, &pan);
		sdp_set_service_classes(record, svclass);

		sdp_uuid16_create(&profile[0].uuid, GN_PROFILE_ID);
		profile[0].version = 0x0100;
		pfseq = sdp_list_append(NULL, &profile[0]);
		sdp_set_profile_descs(record, pfseq);

		sdp_set_info_attr(record, name, NULL, desc);
		break;
	case BNEP_SVC_PANU:
		sdp_uuid16_create(&pan, PANU_SVCLASS_ID);
		svclass = sdp_list_append(NULL, &pan);
		sdp_set_service_classes(record, svclass);

		sdp_uuid16_create(&profile[0].uuid, PANU_PROFILE_ID);
		profile[0].version = 0x0100;
		pfseq = sdp_list_append(NULL, &profile[0]);
		sdp_set_profile_descs(record, pfseq);

		sdp_set_info_attr(record, name, NULL, desc);
		break;
	default:
		return NULL;
	}

	sdp_uuid16_create(&root_uuid, PUBLIC_BROWSE_GROUP);
	root = sdp_list_append(NULL, &root_uuid);
	sdp_set_browse_groups(record, root);

	sdp_uuid16_create(&l2cap, L2CAP_UUID);
	proto[0] = sdp_list_append(NULL, &l2cap);
	p = sdp_data_alloc(SDP_UINT16, &psm);
	proto[0] = sdp_list_append(proto[0], p);
	apseq    = sdp_list_append(NULL, proto[0]);

	sdp_uuid16_create(&bnep, BNEP_UUID);
	proto[1] = sdp_list_append(NULL, &bnep);
	v = sdp_data_alloc(SDP_UINT16, &version);
	proto[1] = sdp_list_append(proto[1], v);

	/* Supported protocols */
	{
		uint16_t ptype[] = {
			0x0800,  /* IPv4 */
			0x0806,  /* ARP */
		};
		sdp_data_t *head, *pseq;
		int p;

		for (p = 0, head = NULL; p < 2; p++) {
			sdp_data_t *data = sdp_data_alloc(SDP_UINT16, &ptype[p]);
			if (head)
				sdp_seq_append(head, data);
			else
				head = data;
		}
		pseq = sdp_data_alloc(SDP_SEQ16, head);
		proto[1] = sdp_list_append(proto[1], pseq);
	}

	apseq = sdp_list_append(apseq, proto[1]);

	aproto = sdp_list_append(NULL, apseq);
	sdp_set_access_protos(record, aproto);

	add_lang_attr(record);

	sdp_attr_add_new(record, SDP_ATTR_SECURITY_DESC,
				SDP_UINT16, &security_desc);

	sdp_data_free(p);
	sdp_data_free(v);
	sdp_list_free(apseq, NULL);
	sdp_list_free(root, NULL);
	sdp_list_free(aproto, NULL);
	sdp_list_free(proto[0], NULL);
	sdp_list_free(proto[1], NULL);
	sdp_list_free(svclass, NULL);
	sdp_list_free(pfseq, NULL);

	return record;
}

static ssize_t send_bnep_ctrl_rsp(int sk, uint16_t response)
{
	struct bnep_control_rsp rsp;

	rsp.type = BNEP_CONTROL;
	rsp.ctrl = BNEP_SETUP_CONN_RSP;
	rsp.resp = htons(response);

	return send(sk, &rsp, sizeof(rsp), 0);
}

static void cancel_authorization(struct setup_session *s)
{
	DBusMessage *msg;
	const char *uuid;

	msg = dbus_message_new_method_call("org.bluez", "/org/bluez",
						"org.bluez.Database",
						"CancelAuthorizationRequest");
	if (!msg) {
		error("Unable to allocate new method call");
		return;
	}

	uuid = bnep_uuid(s->dst_role);
	dbus_message_append_args(msg,
			DBUS_TYPE_STRING, &s->address,
			DBUS_TYPE_STRING, &uuid,
			DBUS_TYPE_INVALID);

	send_message_and_unref(connection, msg);
}

static void authorization_callback(DBusPendingCall *pcall, void *data)
{
	struct setup_session *s = data;
	struct network_server *ns = NULL;
	DBusMessage *reply = dbus_pending_call_steal_reply(pcall);
	char path[MAX_PATH_LENGTH], devname[16];
	uint16_t response = BNEP_CONN_NOT_ALLOWED;
	DBusError derr;
	const char *bridge;

	if (!g_slist_find(setup_sessions, s)) {
		dbus_message_unref(reply);
		return;
	}

	snprintf(path, MAX_PATH_LENGTH, NETWORK_PATH"/%s", bnep_name(s->dst_role));
	dbus_connection_get_object_user_data(connection, path, (void *) &ns);

	/* Server can be disabled in the meantime */
	if (ns == NULL || ns->enable == FALSE)
		goto failed;

	dbus_error_init(&derr);
	if (dbus_set_error_from_message(&derr, reply)) {
		error("Access denied: %s", derr.message);
		if (dbus_error_has_name(&derr, DBUS_ERROR_NO_REPLY)) {
			debug("Canceling authorization request");
			cancel_authorization(s);
		}
		dbus_error_free(&derr);
		goto failed;
	}

	memset(devname, 0, 16);
	strncpy(devname, prefix, strlen(prefix));

	if (bnep_connadd(s->nsk, s->dst_role, devname) < 0)
		goto failed;

	info("Authorization succedded. New connection: %s", devname);

	bridge = bridge_get_name(ns->id);
	if (bridge) {
		if (bridge_add_interface(ns->id, devname) < 0) {
			error("Can't add %s to the bridge %s: %s(%d)",
					devname, bridge, strerror(errno),
					errno);
			goto failed;
		}

		bnep_if_up(devname, 0);
	} else
		bnep_if_up(devname, ns->id);

	response = BNEP_SUCCESS;

	ns->clients = g_slist_append(ns->clients, g_strdup(s->address));

failed:
	send_bnep_ctrl_rsp(s->nsk, response);
	dbus_message_unref(reply);
}

static void setup_watch_destroy(void *data)
{
	struct setup_session *s;
	GSList *l;

	/*
	 * Remote initiated: socket HUP
	 * Authorization: denied/accepted
	 */
	l = g_slist_find(setup_sessions, data);
	if (!l)
		return;

	s = l->data;

	setup_sessions = g_slist_remove(setup_sessions, s);

	/* Remove active watches */
	if (s->watch)
		g_source_remove(s->watch);
	if (s->timeout)
		g_source_remove(s->timeout);
	setup_free(s);
}

static int authorize_connection(struct setup_session *s)
{
	DBusMessage *msg;
	DBusPendingCall *pending;
	const char *uuid;

	msg = dbus_message_new_method_call("org.bluez", "/org/bluez",
				"org.bluez.Database", "RequestAuthorization");
	if (!msg) {
		error("Unable to allocat new RequestAuthorization method call");
		return -ENOMEM;
	}

	uuid = bnep_uuid(s->dst_role);
	debug("Requesting authorization for %s UUID:%s", s->address, uuid);

	dbus_message_append_args(msg,
			DBUS_TYPE_STRING, &s->address,
			DBUS_TYPE_STRING, &uuid,
			DBUS_TYPE_INVALID);

	if (dbus_connection_send_with_reply(connection,
				msg, &pending, -1) == FALSE) {
		error("Sending of authorization request failed");
		dbus_message_unref(msg);
		return -EACCES;
	}

	dbus_pending_call_set_notify(pending,
			authorization_callback, s, setup_watch_destroy);
	dbus_pending_call_unref(pending);
	dbus_message_unref(msg);

	return 0;
}

static uint16_t inline chk_role(uint16_t dst_role, uint16_t src_role)
{
	/* Allowed PAN Profile scenarios */
	switch (dst_role) {
	case BNEP_SVC_NAP:
	case BNEP_SVC_GN:
		if (src_role == BNEP_SVC_PANU)
			return 0;
		return BNEP_CONN_INVALID_SRC;
	case BNEP_SVC_PANU:
		if (src_role == BNEP_SVC_PANU ||
			src_role == BNEP_SVC_GN ||
			src_role == BNEP_SVC_NAP)
			return 0;

		return BNEP_CONN_INVALID_SRC;
	}

	return BNEP_CONN_INVALID_DST;
}

static gboolean connect_setup_event(GIOChannel *chan,
					GIOCondition cond, gpointer data)
{
	struct setup_session *s = data;
	struct network_server *ns = NULL;
	struct bnep_setup_conn_req *req;
	unsigned char pkt[BNEP_MTU];
	char path[MAX_PATH_LENGTH];
	uint16_t response;
	uint8_t *pservice;
	ssize_t r;
	int sk;

	if (cond & G_IO_NVAL)
		return FALSE;

	if (cond & (G_IO_ERR | G_IO_HUP)) {
		error("Hangup or error on BNEP socket");
		/* If there is a pending authorization */
		if (s->attempts)
			cancel_authorization(s);
		return FALSE;
	}

	sk = g_io_channel_unix_get_fd(chan);
	memset(pkt, 0, sizeof(pkt));
	r = recv(sk, pkt, sizeof(pkt) - 1, 0);

	req = (struct bnep_setup_conn_req *) pkt;
	/*
	 * FIXME: According to BNEP SPEC the UUID size can be
	 * 2-16 bytes. Currently only 2 bytes size is supported
	 */
	if (req->uuid_size != 2 || r != (sizeof(*req) + req->uuid_size * 2)) {
		error("Invalid BNEP packet size");
		response = BNEP_CONN_INVALID_SVC;
		goto reply;
	}

	if (req->type != BNEP_CONTROL || req->ctrl != BNEP_SETUP_CONN_REQ) {
		error("Invalid BNEP control packet content");
		return FALSE;
	}

	pservice = req->service;
	/* Getting destination service: considering 2 bytes size */
	s->dst_role = ntohs(bt_get_unaligned((uint16_t *) pservice));
	pservice += req->uuid_size;
	/* Getting source service: considering 2 bytes size */
	s->src_role = ntohs(bt_get_unaligned((uint16_t *) pservice));

	response = chk_role(s->src_role, s->dst_role);
	if (response)
		goto reply;

	snprintf(path, MAX_PATH_LENGTH, NETWORK_PATH"/%s", bnep_name(s->dst_role));
	dbus_connection_get_object_user_data(connection, path, (void *) &ns);

	if (ns == NULL || ns->enable == FALSE) {
		response = BNEP_CONN_NOT_ALLOWED;
		goto reply;
	}

	if (s->timeout) {
		g_source_remove(s->timeout);
		s->timeout = 0;
	}

	if (++s->attempts > MAX_SETUP_ATTEMPTS) {
		/* Retransmission */
		response = BNEP_CONN_NOT_ALLOWED;
		goto reply;
	}

	/* Wait authorization before reply success */
	if (authorize_connection(s) < 0) {
		response = BNEP_CONN_NOT_ALLOWED;
		goto reply;
	}

	return TRUE;
reply:
	send_bnep_ctrl_rsp(sk, response);
	return FALSE;
}

static gboolean setup_timeout(void *data)
{
	setup_watch_destroy(data);
	return FALSE;
}

static gboolean connect_event(GIOChannel *chan,
				GIOCondition cond, gpointer data)
{
	struct sockaddr_l2 addr;
	struct setup_session *s;
	GIOChannel *io;
	socklen_t addrlen;
	char peer[18];
	bdaddr_t dst;
	unsigned short psm;
	int sk, nsk;

	if (cond & G_IO_NVAL)
		return FALSE;

	if (cond & (G_IO_ERR | G_IO_HUP)) {
		error("Hangup or error on L2CAP socket PSM 15");
		g_io_channel_close(chan);
		return FALSE;
	}

	sk = g_io_channel_unix_get_fd(chan);

	memset(&addr, 0, sizeof(addr));
	addrlen = sizeof(addr);

	nsk = accept(sk, (struct sockaddr *) &addr, &addrlen);
	if (nsk < 0)
		return TRUE;

	bacpy(&dst, &addr.l2_bdaddr);
	psm = btohs(addr.l2_psm);
	ba2str(&dst, peer);

	info("Connection from: %s on PSM %d", peer, psm);

	if (g_slist_find_custom(setup_sessions, peer,
				(GCompareFunc) setup_cmp)) {
		error("Pending connection setup session");
		close(nsk);
		return TRUE;
	}

	s = g_new0(struct setup_session, 1);
	s->address = g_strdup(peer);
	s->nsk = nsk;

	io = g_io_channel_unix_new(nsk);
	g_io_channel_set_close_on_unref(io, TRUE);
	/* New watch for BNEP setup */
	s->watch = g_io_add_watch_full(io, G_PRIORITY_DEFAULT,
			G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
			connect_setup_event, s, &setup_watch_destroy);
	g_io_channel_unref(io);

	/* Remove the timeout at the first valid msg */
	s->timeout = g_timeout_add(SETUP_TIMEOUT, setup_timeout, s);

	setup_sessions = g_slist_append(setup_sessions, s);

	return TRUE;
}

int server_init(DBusConnection *conn, const char *iface_prefix,
		gboolean secure)
{
	struct l2cap_options l2o;
	struct sockaddr_l2 l2a;
	socklen_t olen;
	int sk, lm, err;

	/* Create L2CAP socket and bind it to PSM BNEP */
	sk = socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
	if (sk < 0) {
		err = errno;
		error("Cannot create L2CAP socket. %s(%d)",
					strerror(err), err);
		return -err;
	}

	memset(&l2a, 0, sizeof(l2a));
	l2a.l2_family = AF_BLUETOOTH;
	bacpy(&l2a.l2_bdaddr, BDADDR_ANY);
	l2a.l2_psm = htobs(BNEP_PSM);

	if (bind(sk, (struct sockaddr *) &l2a, sizeof(l2a))) {
		err = errno;
		error("Bind failed. %s(%d)", strerror(err), err);
		goto fail;
	}

	/* Setup L2CAP options according to BNEP spec */
	memset(&l2o, 0, sizeof(l2o));
	olen = sizeof(l2o);
	if (getsockopt(sk, SOL_L2CAP, L2CAP_OPTIONS, &l2o, &olen) < 0) {
		err = errno;
		error("Failed to get L2CAP options. %s(%d)",
					strerror(err), err);
		goto fail;
	}

	l2o.imtu = l2o.omtu = BNEP_MTU;
	if (setsockopt(sk, SOL_L2CAP, L2CAP_OPTIONS, &l2o, sizeof(l2o)) < 0) {
		err = errno;
		error("Failed to set L2CAP options. %s(%d)",
					strerror(err), err);
		goto fail;
	}

	lm = secure ? L2CAP_LM_SECURE : 0;
	if (lm && setsockopt(sk, SOL_L2CAP, L2CAP_LM, &lm, sizeof(lm)) < 0) {
		err = errno;
		error("Failed to set link mode. %s(%d)",
					strerror(err), err);
		goto fail;
	}
	security = secure;

	if (listen(sk, 1) < 0) {
		err = errno;
		error("Listen failed. %s(%d)", strerror(err), err);
		goto fail;
	}

	connection = dbus_connection_ref(conn);
	prefix = iface_prefix;

	bnep_io = g_io_channel_unix_new(sk);
	g_io_channel_set_close_on_unref(bnep_io, FALSE);
	g_io_add_watch(bnep_io, G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
							connect_event, NULL);

	if (bridge_create(BNEP_SVC_GN) < 0)
		error("Can't create GN bridge");

	return 0;
fail:

	close(sk);
	errno = err;
	return -err;
}

void server_exit()
{
	if (setup_sessions) {
		g_slist_foreach(setup_sessions, (GFunc) setup_free, NULL);
		g_slist_free(setup_sessions);
		setup_sessions = NULL;
	}

	if (bnep_io != NULL) {
		g_io_channel_close(bnep_io);
		g_io_channel_unref(bnep_io);
		bnep_io = NULL;
	}

	if (bridge_remove(BNEP_SVC_GN) < 0)
		error("Can't remove GN bridge");

	dbus_connection_unref(connection);
	connection = NULL;
}

uint32_t register_server_record(struct network_server *ns)
{
	sdp_record_t *record;

	record = server_record_new(ns->name, ns->id);
	if (!record) {
		error("Unable to allocate new service record");
		return 0;
	}

	if (add_record_to_server(&ns->src, record) < 0) {
		error("Failed to register service record");
		sdp_record_free(record);
		return 0;
	}

	debug("register_server_record: got record id 0x%x", record->handle);

	return record->handle;
}

static DBusHandlerResult get_uuid(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct network_server *ns = data;
	DBusMessage *reply;
	const char *uuid;

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	uuid = bnep_uuid(ns->id);
	dbus_message_append_args(reply,
			DBUS_TYPE_STRING, &uuid,
			DBUS_TYPE_INVALID);

	return send_message_and_unref(conn, reply);
}

static DBusHandlerResult enable(DBusConnection *conn,
				DBusMessage *msg, void *data)
{
	struct network_server *ns = data;
	DBusMessage *reply;

	if (ns->enable)
		return error_already_exists(conn, msg, "Server already enabled");

	if (bacmp(&ns->src, BDADDR_ANY) == 0) {
		int dev_id;

		dev_id = hci_get_route(&ns->src);
		if ((dev_id < 0) || (hci_devba(dev_id, &ns->src) < 0))
			return error_failed(conn, msg, "Adapter not available");

		/* Store the server info */
		server_store(ns->path);
	}

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	/* Add the service record */
	ns->record_id = register_server_record(ns);
	if (!ns->record_id) {
		dbus_message_unref(reply);
		return error_failed(conn, msg,
				"service record registration failed");
	}

	ns->enable = TRUE;

	store_property(&ns->src, ns->id, "enabled", "1");

	dbus_connection_emit_signal(conn, ns->path, NETWORK_SERVER_INTERFACE,
					"Enabled", DBUS_TYPE_INVALID);

	return send_message_and_unref(conn, reply);
}

static void kill_connection(void *data, void *udata)
{
	const char *address = data;
	bdaddr_t dst;

	str2ba(address, &dst);
	bnep_kill_connection(&dst);
}

static DBusHandlerResult disable(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct network_server *ns = data;
	DBusMessage *reply;

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	if (!ns->enable)
		return error_failed(conn, msg, "Not enabled");

	/* Remove the service record */
	if (ns->record_id) {
		remove_record_from_server(ns->record_id);
		ns->record_id = 0;
	}

	ns->enable = FALSE;

	g_slist_foreach(ns->clients, (GFunc) kill_connection, NULL);

	store_property(&ns->src, ns->id, "enabled", "0");

	dbus_connection_emit_signal(conn, ns->path, NETWORK_SERVER_INTERFACE,
					"Disabled", DBUS_TYPE_INVALID);

	return send_message_and_unref(conn, reply);
}

static DBusHandlerResult is_enabled(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	struct network_server *ns = data;
	DBusMessage *reply;

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	dbus_message_append_args(reply, DBUS_TYPE_BOOLEAN, &ns->enable,
					DBUS_TYPE_INVALID);

	return send_message_and_unref(conn, reply);
}

static DBusHandlerResult set_name(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct network_server *ns = data;
	DBusMessage *reply;
	DBusError derr;
	const char *name;

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	dbus_error_init(&derr);
	if (!dbus_message_get_args(msg, &derr,
				DBUS_TYPE_STRING, &name,
				DBUS_TYPE_INVALID)) {
		error_invalid_arguments(conn, msg, derr.message);
		dbus_error_free(&derr);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (!name || (strlen(name) == 0))
		return error_invalid_arguments(conn, msg, "Invalid name");

	if (ns->name)
		g_free(ns->name);
	ns->name = g_strdup(name);

	if (ns->enable && ns->record_id) {
		uint32_t handle = register_server_record(ns);
		if (!handle) {
			dbus_message_unref(reply);
			return error_failed(conn, msg,
				"Service record attribute update failed");
		}

		remove_record_from_server(ns->record_id);
		ns->record_id = handle;
	}

	store_property(&ns->src, ns->id, "name", ns->name);

	return send_message_and_unref(conn, reply);
}

static DBusHandlerResult get_name(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct network_server *ns = data;
	char name[] = "";
	const char *pname = (ns->name ? ns->name : name);
	DBusMessage *reply;

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	dbus_message_append_args(reply,
			DBUS_TYPE_STRING, &pname,
			DBUS_TYPE_INVALID);

	return send_message_and_unref(conn, reply);
}

static DBusHandlerResult set_address_range(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static DBusHandlerResult set_routing(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	struct network_server *ns = data;
	DBusMessage *reply;
	DBusError derr;
	const char *iface;

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	dbus_error_init(&derr);
	if (!dbus_message_get_args(msg, &derr,
				DBUS_TYPE_STRING, &iface,
				DBUS_TYPE_INVALID)) {
		error_invalid_arguments(conn, msg, derr.message);
		dbus_error_free(&derr);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	/* FIXME: Check if the interface is valid/UP */
	if (!iface || (strlen(iface) == 0))
		return error_invalid_arguments(conn, msg, "Invalid interface");

	if (ns->iface)
		g_free(ns->iface);
	ns->iface = g_strdup(iface);

	return send_message_and_unref(conn, reply);
}

static DBusHandlerResult get_info(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct network_server *ns = data;
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter dict;
	const char *uuid;

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
			DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
			DBUS_TYPE_STRING_AS_STRING DBUS_TYPE_VARIANT_AS_STRING
			DBUS_DICT_ENTRY_END_CHAR_AS_STRING, &dict);

	dbus_message_iter_append_dict_entry(&dict, "name",
			DBUS_TYPE_STRING, &ns->name);

	uuid = bnep_uuid(ns->id);
	dbus_message_iter_append_dict_entry(&dict, "uuid",
			DBUS_TYPE_STRING, &uuid);

	dbus_message_iter_close_container(&iter, &dict);

	return send_message_and_unref(conn, reply);
}

static void server_free(struct network_server *ns)
{
	if (!ns)
		return;

	/* FIXME: Missing release/free all bnepX interfaces */
	if (ns->record_id)
		remove_record_from_server(ns->record_id);

	if (ns->iface)
		g_free(ns->iface);

	if (ns->name)
		g_free(ns->name);

	if (ns->range)
		g_free(ns->range);

	if (ns->path)
		g_free(ns->path);

	if (ns->clients) {
		g_slist_foreach(ns->clients, (GFunc) g_free, NULL);
		g_slist_free(ns->clients);
	}

	g_free(ns);
}

static void server_unregister(DBusConnection *conn, void *data)
{
	struct network_server *ns = data;

	info("Unregistered server path:%s", ns->path);

	server_free(ns);
}

static DBusMethodVTable server_methods[] = {
	{ "GetUUID",		get_uuid,		"",	"s"	},
	{ "Enable",		enable,			"",	""	},
	{ "Disable",		disable,		"",	""	},
	{ "IsEnabled",		is_enabled,		"",	"b"	},
	{ "SetName",		set_name,		"s",	""	},
	{ "GetName",		get_name,		"",	"s"	},
	{ "SetAddressRange",	set_address_range,	"ss",	""	},
	{ "SetRouting",		set_routing,		"s",	""	},
	{ "GetInfo",		get_info,		"",	"a{sv}"	},
	{ NULL, NULL, NULL, NULL }
};

static DBusSignalVTable server_signals[] = {
	{ "Enabled",	""	},
	{ "Disabled",	""	},
	{ NULL, NULL }
};

int server_register(const char *path, bdaddr_t *src, uint16_t id)
{
	struct network_server *ns;

	if (!path)
		return -EINVAL;

	ns = g_new0(struct network_server, 1);

	if (!dbus_connection_create_object_path(connection, path, ns,
						server_unregister)) {
		error("D-Bus failed to register %s path", path);
		server_free(ns);
		return -1;
	}

	if (!dbus_connection_register_interface(connection, path,
						NETWORK_SERVER_INTERFACE,
						server_methods,
						server_signals, NULL)) {
		error("D-Bus failed to register %s interface",
				NETWORK_SERVER_INTERFACE);
		dbus_connection_destroy_object_path(connection, path);
		return -1;
	}

	/* Setting a default name */
	if (id == BNEP_SVC_NAP)
		ns->name = g_strdup("BlueZ NAP service");
	else if (id == BNEP_SVC_GN)
		ns->name = g_strdup("BlueZ GN service");
	else
		ns->name = g_strdup("BlueZ PANU service");

	ns->path = g_strdup(path);
	ns->id = id;
	bacpy(&ns->src, src);

	info("Registered server path:%s", path);

	return 0;
}

int server_register_from_file(const char *path, const bdaddr_t *src,
		uint16_t id, const char *filename)
{
	struct network_server *ns;
	char *str;

	if (!path)
		return -EINVAL;

	ns = g_new0(struct network_server, 1);

	bacpy(&ns->src, src);
	ns->path = g_strdup(path);
	ns->id = id;
	ns->name = textfile_get(filename, "name");
	if (!ns->name) {
		/* Name is mandatory */
		server_free(ns);
		return -1;
	}

	ns->range = textfile_get(filename, "address_range");
	ns->iface = textfile_get(filename, "routing");

	str = textfile_get(filename, "enabled");
	if (str) {
		if (strcmp("1", str) == 0) {
			ns->record_id = register_server_record(ns);
			ns->enable = TRUE;
		}
		g_free(str);
	}

	if (!dbus_connection_create_object_path(connection, path, ns,
						server_unregister)) {
		error("D-Bus failed to register %s path", path);
		server_free(ns);
		return -1;
	}

	if (!dbus_connection_register_interface(connection, path,
						NETWORK_SERVER_INTERFACE,
						server_methods,
						server_signals, NULL)) {
		error("D-Bus failed to register %s interface",
				NETWORK_SERVER_INTERFACE);
		dbus_connection_destroy_object_path(connection, path);
		return -1;
	}

	info("Registered server path:%s", path);

	return 0;
}

int server_store(const char *path)
{
	struct network_server *ns;
	char filename[PATH_MAX + 1];
	char addr[18];

	if (!dbus_connection_get_object_user_data(connection,
				path, (void *) &ns)) {
		error("Unable to salve %s on storage", path);
		return -ENOENT;
	}

	ba2str(&ns->src, addr);
	if (ns->id == BNEP_SVC_NAP)
		create_name(filename, PATH_MAX, STORAGEDIR, addr, "nap");
	else if (ns->id == BNEP_SVC_GN)
		create_name(filename, PATH_MAX, STORAGEDIR, addr, "gn");
	else
		create_name(filename, PATH_MAX, STORAGEDIR, addr, "panu");

	create_file(filename, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

	textfile_put(filename, "name", ns->name);

	if (ns->iface)
		textfile_put(filename, "routing", ns->iface);

	if (ns->range)
		textfile_put(filename, "range", ns->range);

	textfile_put(filename, "enabled", ns->enable ? "1": "0");

	return 0;
}

int server_find_data(const char *path, const char *pattern)
{
	struct network_server *ns;
	const char *uuid;

	if (!dbus_connection_get_object_user_data(connection, path, (void *) &ns))
		return -1;

	if (ns->name && strcasecmp(pattern, ns->name) == 0)
		return 0;

	if (ns->iface && strcasecmp(pattern, ns->iface) == 0)
		return 0;

	uuid = bnep_name(ns->id);
	if (uuid && strcasecmp(pattern, uuid) == 0)
		return 0;

	if (bnep_service_id(pattern) == ns->id)
		return 0;

	return -1;
}
