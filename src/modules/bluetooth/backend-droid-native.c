/***
  This file is part of PulseAudio.

  Copyright 2014 Wim Taymans <wim.taymans at gmail.com>
            2017 Jolla Ltd.
                Contact: Juho Hämäläinen <juho.hamalainen@jolla.com>

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with PulseAudio; if not, see <http://www.gnu.org/licenses/>.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pulsecore/core.h>
#include <pulsecore/shared.h>
#include <pulsecore/core-error.h>
#include <pulsecore/core-util.h>
#include <pulsecore/dbus-shared.h>
#include <pulsecore/idxset.h>
#include <pulsecore/hashmap.h>
#include <pulsecore/log.h>
#include <pulse/timeval.h>
#include <pulse/rtclock.h>

#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/sco.h>

#include "bluez5-util.h"

struct transport_rfcomm;

struct pa_bluetooth_backend {
  pa_core *core;
  pa_dbus_connection *connection;
  pa_bluetooth_discovery *discovery;
  pa_droid_volume_control *volume_control;
  struct transport_rfcomm *trfc;
  pa_hashmap *call_paths;
  pa_hashmap *active_calls;
  pa_hashmap *held_calls;
  const char *incoming_call_path;

  PA_LLIST_HEAD(pa_dbus_pending, pending);
};

struct transport_rfcomm {
    int rfcomm_fd;
    pa_io_event *rfcomm_io;
    pa_mainloop_api *mainloop;
    pa_bluetooth_backend *backend;
    pa_time_event *ring_time_event;
};

#define BLUEZ_SERVICE "org.bluez"
#define BLUEZ_MEDIA_TRANSPORT_INTERFACE BLUEZ_SERVICE ".MediaTransport1"

#define BLUEZ_ERROR_NOT_SUPPORTED "org.bluez.Error.NotSupported"

#define BLUEZ_PROFILE_MANAGER_INTERFACE BLUEZ_SERVICE ".ProfileManager1"
#define BLUEZ_PROFILE_INTERFACE BLUEZ_SERVICE ".Profile1"

#define HSP_AG_PROFILE "/Profile/HSPAGProfile"

#define PROFILE_INTROSPECT_XML                                          \
    DBUS_INTROSPECT_1_0_XML_DOCTYPE_DECL_NODE                           \
    "<node>"                                                            \
    " <interface name=\"" BLUEZ_PROFILE_INTERFACE "\">"                 \
    "  <method name=\"Release\">"                                       \
    "  </method>"                                                       \
    "  <method name=\"RequestDisconnection\">"                          \
    "   <arg name=\"device\" direction=\"in\" type=\"o\"/>"             \
    "  </method>"                                                       \
    "  <method name=\"NewConnection\">"                                 \
    "   <arg name=\"device\" direction=\"in\" type=\"o\"/>"             \
    "   <arg name=\"fd\" direction=\"in\" type=\"h\"/>"                 \
    "   <arg name=\"opts\" direction=\"in\" type=\"a{sv}\"/>"           \
    "  </method>"                                                       \
    " </interface>"                                                     \
    " <interface name=\"org.freedesktop.DBus.Introspectable\">"         \
    "  <method name=\"Introspect\">"                                    \
    "   <arg name=\"data\" type=\"s\" direction=\"out\"/>"              \
    "  </method>"                                                       \
    " </interface>"                                                     \
    "</node>"

#define OFONO_SERVICE                       "org.ofono"
#define OFONO_MANAGER_INTERFACE             OFONO_SERVICE ".Manager"
#define OFONO_VOICECALL_INTERFACE           OFONO_SERVICE ".VoiceCall"
#define OFONO_VOICECALL_MANAGER_INTERFACE   OFONO_SERVICE ".VoiceCallManager"

#define RING_WAIT_TIME ((pa_usec_t) (3 * PA_USEC_PER_SEC))

static void rfcomm_ring_start(struct transport_rfcomm *trfc);
static void rfcomm_ring_stop(struct transport_rfcomm *trfc);
static void voicecall_get_all_calls(pa_bluetooth_backend *backend);
static void voicecall_clear_calls(pa_bluetooth_backend *backend);

static pa_dbus_pending* send_and_add_to_pending(pa_bluetooth_backend *backend, DBusMessage *m,
        DBusPendingCallNotifyFunction func, void *call_data) {

    pa_dbus_pending *p;
    DBusPendingCall *call;

    pa_assert(backend);
    pa_assert(m);

    pa_assert_se(dbus_connection_send_with_reply(pa_dbus_connection_get(backend->connection), m, &call, -1));

    p = pa_dbus_pending_new(pa_dbus_connection_get(backend->connection), m, call, backend, call_data);
    PA_LLIST_PREPEND(pa_dbus_pending, backend->pending, p);
    dbus_pending_call_set_notify(call, func, p, NULL);

    return p;
}

static void voicecall_send(pa_bluetooth_backend *backend, const char *path, const char *action) {
    DBusMessage *m;

    m = dbus_message_new_method_call(OFONO_SERVICE, path, OFONO_VOICECALL_INTERFACE, action);
    dbus_connection_send(pa_dbus_connection_get(backend->connection), m, NULL);
}

static char *path_get_modem(const char *path) {
    char *modem;
    char *d;

    if (!path || strlen(path) < 2)
        return NULL;

    modem = pa_xstrdup(path);
    if ((d = strstr(modem + 1, "/"))) {
        d[0] = '\0';
        return modem;
    }

    pa_xfree(modem);
    return NULL;
}

static void voicecall_hold_and_answer(pa_bluetooth_backend *backend,  const char *path) {
    DBusMessage *m;
    char *modem;

    if ((modem = path_get_modem(path))) {
        m = dbus_message_new_method_call(OFONO_SERVICE, modem, OFONO_VOICECALL_MANAGER_INTERFACE, "HoldAndAnswer");
        dbus_connection_send(pa_dbus_connection_get(backend->connection), m, NULL);
        pa_xfree(modem);
    }
}

static void voicecall_swap_calls(pa_bluetooth_backend *backend,  const char *path) {
    DBusMessage *m;
    char *modem;

    if ((modem = path_get_modem(path))) {
        m = dbus_message_new_method_call(OFONO_SERVICE, modem, OFONO_VOICECALL_MANAGER_INTERFACE, "SwapCalls");
        dbus_connection_send(pa_dbus_connection_get(backend->connection), m, NULL);
        pa_xfree(modem);
    }
}

static int bluez5_sco_acquire_cb(pa_bluetooth_transport *t, bool optional, size_t *imtu, size_t *omtu) {
    pa_bluetooth_device *d = t->device;
    struct transport_rfcomm *trfc = t->userdata;
    struct sockaddr_sco addr;
    int err, i;
    int sock;
    bdaddr_t src;
    bdaddr_t dst;
    const char *src_addr, *dst_addr;

    src_addr = d->adapter->address;
    dst_addr = d->address;

    /* don't use ba2str to avoid -lbluetooth */
    for (i = 5; i >= 0; i--, src_addr += 3)
        src.b[i] = strtol(src_addr, NULL, 16);
    for (i = 5; i >= 0; i--, dst_addr += 3)
        dst.b[i] = strtol(dst_addr, NULL, 16);

    sock = socket(PF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_SCO);
    if (sock < 0) {
        pa_log_error("socket(SEQPACKET, SCO) %s", pa_cstrerror(errno));
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sco_family = AF_BLUETOOTH;
    bacpy(&addr.sco_bdaddr, &src);

    if (bind(sock, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        pa_log_error("bind(): %s", pa_cstrerror(errno));
        goto fail_close;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sco_family = AF_BLUETOOTH;
    bacpy(&addr.sco_bdaddr, &dst);

    pa_log_info ("doing connect\n");
    err = connect(sock, (struct sockaddr *) &addr, sizeof(addr));
    if (err < 0 && !(errno == EAGAIN || errno == EINPROGRESS)) {
        pa_log_error("connect(): %s", pa_cstrerror(errno));
        goto fail_close;
    }

    /* The "48" below is hardcoded until we get meaningful MTU values exposed
     * by the kernel */

    if (imtu)
        *imtu = 48;

    if (omtu)
        *omtu = 48;

    pa_droid_volume_control_acquire(trfc->backend->volume_control, t);

    return sock;

fail_close:
    close(sock);
    return -1;
}

static void bluez5_sco_release_cb(pa_bluetooth_transport *t) {
    struct transport_rfcomm *trfc;

    trfc = t->userdata;

    pa_droid_volume_control_release(trfc->backend->volume_control);

    pa_log_info("Transport %s released", t->path);
    /* device will close the SCO socket for us */
}

static void register_profile_reply(DBusPendingCall *pending, void *userdata) {
    DBusMessage *r;
    pa_dbus_pending *p;
    pa_bluetooth_backend *b;
    char *profile;

    pa_assert(pending);
    pa_assert_se(p = userdata);
    pa_assert_se(b = p->context_data);
    pa_assert_se(profile = p->call_data);
    pa_assert_se(r = dbus_pending_call_steal_reply(pending));

    if (dbus_message_is_error(r, BLUEZ_ERROR_NOT_SUPPORTED)) {
        pa_log_info("Couldn't register profile %s because it is disabled in BlueZ", profile);
        goto finish;
    }

    if (dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR) {
        pa_log_error(BLUEZ_PROFILE_MANAGER_INTERFACE ".RegisterProfile() failed: %s: %s", dbus_message_get_error_name(r),
                     pa_dbus_get_error_message(r));
        goto finish;
    }

finish:
    dbus_message_unref(r);

    PA_LLIST_REMOVE(pa_dbus_pending, b->pending, p);
    pa_dbus_pending_free(p);

    pa_xfree(profile);
}

static void register_profile(pa_bluetooth_backend *b, const char *profile, const char *uuid) {
    DBusMessage *m;
    DBusMessageIter i, d;

    pa_log_debug("Registering Profile %s", profile);

    pa_assert_se(m = dbus_message_new_method_call(BLUEZ_SERVICE, "/org/bluez", BLUEZ_PROFILE_MANAGER_INTERFACE, "RegisterProfile"));

    dbus_message_iter_init_append(m, &i);
    dbus_message_iter_append_basic(&i, DBUS_TYPE_OBJECT_PATH, &profile);
    dbus_message_iter_append_basic(&i, DBUS_TYPE_STRING, &uuid);
    dbus_message_iter_open_container(&i, DBUS_TYPE_ARRAY, DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING DBUS_TYPE_STRING_AS_STRING
            DBUS_TYPE_VARIANT_AS_STRING DBUS_DICT_ENTRY_END_CHAR_AS_STRING, &d);
    dbus_message_iter_close_container(&i, &d);

    send_and_add_to_pending(b, m, register_profile_reply, pa_xstrdup(profile));
}

static void rfcomm_handle_button(pa_bluetooth_backend *backend) {
    pa_assert(backend);

    if (backend->incoming_call_path) {
        if (pa_hashmap_size(backend->call_paths) == 1) {
            pa_log_debug("answer incoming %s", backend->incoming_call_path);
            voicecall_send(backend, backend->incoming_call_path, "Answer");
        } else {
            pa_log_debug("hold active calls and answer incoming %s", backend->incoming_call_path);
            voicecall_hold_and_answer(backend, backend->incoming_call_path);
        }
    } else if (pa_hashmap_size(backend->active_calls)) {
        pa_log_debug("hangup active call %s", (char *) pa_hashmap_last(backend->active_calls));
        voicecall_send(backend, pa_hashmap_last(backend->active_calls), "Hangup");
        if (pa_hashmap_size(backend->held_calls))
            voicecall_swap_calls(backend, pa_hashmap_last(backend->held_calls));
    } else if (pa_hashmap_size(backend->held_calls)) {
        pa_log_debug("hangup held call %s", (char *) pa_hashmap_last(backend->held_calls));
        voicecall_send(backend, pa_hashmap_last(backend->held_calls), "Hangup");
    }
}

static void rfcomm_io_callback(pa_mainloop_api *io, pa_io_event *e, int fd, pa_io_event_flags_t events, void *userdata) {
    pa_bluetooth_transport *t = userdata;

    pa_assert(io);
    pa_assert(t);

    if (events & (PA_IO_EVENT_HANGUP|PA_IO_EVENT_ERROR)) {
        pa_log_info("Lost RFCOMM connection.");
        goto fail;
    }

    if (events & PA_IO_EVENT_INPUT) {
        char buf[512];
        ssize_t len;
        int gain;

        len = read(fd, buf, 511);
        buf[len] = 0;
        pa_log_debug("RFCOMM << %s", buf);

        if (sscanf(buf, "AT+VGS=%d", &gain) == 1) {
          t->speaker_gain = gain;
          pa_hook_fire(pa_bluetooth_discovery_hook(t->device->discovery, PA_BLUETOOTH_HOOK_TRANSPORT_SPEAKER_GAIN_CHANGED), t);

        } else if (sscanf(buf, "AT+VGM=%d", &gain) == 1) {
          t->microphone_gain = gain;
          pa_hook_fire(pa_bluetooth_discovery_hook(t->device->discovery, PA_BLUETOOTH_HOOK_TRANSPORT_MICROPHONE_GAIN_CHANGED), t);
        } else if (pa_startswith(buf, "AT+CKPD=200")) {
            struct transport_rfcomm *trfc = t->userdata;
            rfcomm_handle_button(trfc->backend);
        }

        pa_log_debug("RFCOMM >> OK");

        len = write(fd, "\r\nOK\r\n", 6);

        /* we ignore any errors, it's not critical and real errors should
         * be caught with the HANGUP and ERROR events handled above */
        if (len < 0)
            pa_log_error("RFCOMM write error: %s", pa_cstrerror(errno));
    }

    return;

fail:
    pa_bluetooth_transport_unlink(t);
    pa_bluetooth_transport_free(t);
    return;
}

static void transport_destroy(pa_bluetooth_transport *t) {
    struct transport_rfcomm *trfc = t->userdata;

    voicecall_clear_calls(trfc->backend);
    rfcomm_ring_stop(trfc);
    trfc->backend->trfc = NULL;

    trfc->mainloop->io_free(trfc->rfcomm_io);

    shutdown(trfc->rfcomm_fd, SHUT_RDWR);
    close (trfc->rfcomm_fd);

    pa_xfree(trfc);
}

static void set_speaker_gain(pa_bluetooth_transport *t, uint16_t gain) {
    struct transport_rfcomm *trfc = t->userdata;
    char buf[512];
    ssize_t len, written;

    if (t->speaker_gain == gain)
      return;

    t->speaker_gain = gain;

    len = sprintf(buf, "\r\n+VGS=%d\r\n", gain);
    pa_log_debug("RFCOMM >> +VGS=%d", gain);

    written = write(trfc->rfcomm_fd, buf, len);

    if (written != len)
        pa_log_error("RFCOMM write error: %s", pa_cstrerror(errno));
}

static void set_microphone_gain(pa_bluetooth_transport *t, uint16_t gain) {
    struct transport_rfcomm *trfc = t->userdata;
    char buf[512];
    ssize_t len, written;

    if (t->microphone_gain == gain)
      return;

    t->microphone_gain = gain;

    len = sprintf(buf, "\r\n+VGM=%d\r\n", gain);
    pa_log_debug("RFCOMM >> +VGM=%d", gain);

    written = write (trfc->rfcomm_fd, buf, len);

    if (written != len)
        pa_log_error("RFCOMM write error: %s", pa_cstrerror(errno));
}

static void rfcomm_ring(struct transport_rfcomm *trfc) {
    const char *buf = "\r\nRING\r\n";
    const ssize_t len = strlen(buf);
    ssize_t written;

    pa_log_debug("RFCOMM >> RING");
    written = write(trfc->rfcomm_fd, buf, len);

    if (written != len)
        pa_log_error("RFCOMM write error: %s", pa_cstrerror(errno));
}

static void ring_time_cb(pa_mainloop_api *a, pa_time_event *e, const struct timeval *t, void *userdata) {
    struct transport_rfcomm *trfc = (struct transport_rfcomm*) userdata;

    pa_assert(a);
    pa_assert(e);
    pa_assert(trfc);
    pa_assert(e == trfc->ring_time_event);

    rfcomm_ring(trfc);
    pa_core_rttime_restart(trfc->backend->core, trfc->ring_time_event, pa_rtclock_now() + RING_WAIT_TIME);
}

static void rfcomm_ring_start(struct transport_rfcomm *trfc) {
    pa_assert(trfc);

    if (!trfc->ring_time_event) {
        rfcomm_ring(trfc);
        trfc->ring_time_event = pa_core_rttime_new(trfc->backend->core,
                                                   pa_rtclock_now() + RING_WAIT_TIME,
                                                   ring_time_cb, trfc);
    }
}

static void rfcomm_ring_stop(struct transport_rfcomm *trfc) {
    pa_assert(trfc);

    if (trfc->ring_time_event) {
        trfc->backend->core->mainloop->time_free(trfc->ring_time_event);
        trfc->ring_time_event = NULL;
    }
}

static DBusMessage *profile_new_connection(DBusConnection *conn, DBusMessage *m, void *userdata) {
    pa_bluetooth_backend *b = userdata;
    pa_bluetooth_device *d;
    pa_bluetooth_transport *t;
    pa_bluetooth_profile_t p;
    DBusMessage *r;
    int fd;
    const char *sender, *path, PA_UNUSED *handler;
    DBusMessageIter arg_i;
    char *pathfd;
    struct transport_rfcomm *trfc;

    if (!dbus_message_iter_init(m, &arg_i) || !pa_streq(dbus_message_get_signature(m), "oha{sv}")) {
        pa_log_error("Invalid signature found in NewConnection");
        goto fail;
    }

    handler = dbus_message_get_path(m);
    pa_assert(pa_streq(handler, HSP_AG_PROFILE));

    pa_assert(dbus_message_iter_get_arg_type(&arg_i) == DBUS_TYPE_OBJECT_PATH);
    dbus_message_iter_get_basic(&arg_i, &path);

    d = pa_bluetooth_discovery_get_device_by_path(b->discovery, path);
    if (d == NULL) {
        pa_log_error("Device doesnt exist for %s", path);
        goto fail;
    }

    pa_assert_se(dbus_message_iter_next(&arg_i));

    pa_assert(dbus_message_iter_get_arg_type(&arg_i) == DBUS_TYPE_UNIX_FD);
    dbus_message_iter_get_basic(&arg_i, &fd);

    pa_log_debug("dbus: NewConnection path=%s, fd=%d", path, fd);

    sender = dbus_message_get_sender(m);

    p = PA_BLUETOOTH_PROFILE_DROID_HEADSET_HSP;
    pathfd = pa_sprintf_malloc ("%s/fd%d", path, fd);
    d->transports[p] = t = pa_bluetooth_transport_new(d, sender, pathfd, p, NULL, 0);
    pa_xfree(pathfd);

    t->acquire = bluez5_sco_acquire_cb;
    t->release = bluez5_sco_release_cb;
    t->destroy = transport_destroy;
    t->set_speaker_gain = set_speaker_gain;
    t->set_microphone_gain = set_microphone_gain;

    trfc = pa_xnew0(struct transport_rfcomm, 1);
    trfc->rfcomm_fd = fd;
    trfc->mainloop = b->core->mainloop;
    trfc->rfcomm_io = trfc->mainloop->io_new(b->core->mainloop, fd, PA_IO_EVENT_INPUT|PA_IO_EVENT_HANGUP,
        rfcomm_io_callback, t);
    trfc->backend = b;
    b->trfc = trfc;
    t->userdata =  trfc;

    pa_bluetooth_transport_put(t);
    voicecall_get_all_calls(b);

    pa_log_debug("Transport %s available for profile %s", t->path, pa_bluetooth_profile_to_string(t->profile));

    pa_assert_se(r = dbus_message_new_method_return(m));

    return r;

fail:
    pa_assert_se(r = dbus_message_new_error(m, "org.bluez.Error.InvalidArguments", "Unable to handle new connection"));
    return r;
}

static DBusMessage *profile_request_disconnection(DBusConnection *conn, DBusMessage *m, void *userdata) {
    DBusMessage *r;

    pa_assert_se(r = dbus_message_new_method_return(m));

    return r;
}

static DBusHandlerResult profile_handler(DBusConnection *c, DBusMessage *m, void *userdata) {
    pa_bluetooth_backend *b = userdata;
    DBusMessage *r = NULL;
    const char *path, *interface, *member;

    pa_assert(b);

    path = dbus_message_get_path(m);
    interface = dbus_message_get_interface(m);
    member = dbus_message_get_member(m);

    pa_log_debug("dbus: path=%s, interface=%s, member=%s", path, interface, member);

    if (!pa_streq(path, HSP_AG_PROFILE))
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    if (dbus_message_is_method_call(m, "org.freedesktop.DBus.Introspectable", "Introspect")) {
        const char *xml = PROFILE_INTROSPECT_XML;

        pa_assert_se(r = dbus_message_new_method_return(m));
        pa_assert_se(dbus_message_append_args(r, DBUS_TYPE_STRING, &xml, DBUS_TYPE_INVALID));

    } else if (dbus_message_is_method_call(m, BLUEZ_PROFILE_INTERFACE, "Release")) {
    } else if (dbus_message_is_method_call(m, BLUEZ_PROFILE_INTERFACE, "RequestDisconnection")) {
        r = profile_request_disconnection(c, m, userdata);
    } else if (dbus_message_is_method_call(m, BLUEZ_PROFILE_INTERFACE, "NewConnection"))
        r = profile_new_connection(c, m, userdata);
    else
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    if (r) {
        pa_assert_se(dbus_connection_send(pa_dbus_connection_get(b->connection), r, NULL));
        dbus_message_unref(r);
    }

    return DBUS_HANDLER_RESULT_HANDLED;
}

static void voicecall_parse_call(pa_bluetooth_backend *backend, DBusMessageIter *arg_i) {
    DBusMessageIter array_i;
    const char *call_state = NULL;
    const char *path;
    char *call_path;

    pa_assert(backend);
    pa_assert(arg_i);

    pa_assert(dbus_message_iter_get_arg_type(arg_i) == DBUS_TYPE_OBJECT_PATH);
    dbus_message_iter_get_basic(arg_i, &path);

    pa_assert_se(dbus_message_iter_next(arg_i));
    pa_assert(dbus_message_iter_get_arg_type(arg_i) == DBUS_TYPE_ARRAY);
    dbus_message_iter_recurse(arg_i, &array_i);

    while (dbus_message_iter_get_arg_type(&array_i) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter dict_i;
        const char *entry;

        dbus_message_iter_recurse(&array_i, &dict_i);
        pa_assert(dbus_message_iter_get_arg_type(&dict_i) == DBUS_TYPE_STRING);
        dbus_message_iter_get_basic(&dict_i, &entry);

        if (pa_streq(entry, "State")) {
            DBusMessageIter value_i;
            pa_assert_se(dbus_message_iter_next(&dict_i));
            pa_assert(dbus_message_iter_get_arg_type(&dict_i) == DBUS_TYPE_VARIANT);
            dbus_message_iter_recurse(&dict_i, &value_i);
            dbus_message_iter_get_basic(&value_i, &call_state);
            break;
        }

        dbus_message_iter_next(&array_i);
    }

    pa_log_debug("new call %s: %s", path, call_state ? call_state : "<none>");

    call_path = pa_xstrdup(path);
    if (pa_hashmap_put(backend->call_paths, call_path, call_path)) {
        pa_xfree(call_path);
        call_path = pa_hashmap_get(backend->call_paths, path);
    }

    if (call_state && (pa_streq(call_state, "incoming") || pa_streq(call_state, "waiting"))) {
        backend->incoming_call_path = call_path;
        if (pa_hashmap_size(backend->call_paths) == 1)
            rfcomm_ring_start(backend->trfc);
    } else {
        pa_hashmap_remove(backend->active_calls, call_path);
        pa_hashmap_put(backend->active_calls, call_path, call_path);
    }
}

static void get_calls_reply(DBusPendingCall *pending, void *userdata) {
    DBusMessage *r;
    pa_dbus_pending *p;
    pa_bluetooth_backend *backend;
    DBusMessageIter arg_i, array_i, struct_i;

    pa_assert(pending);
    pa_assert_se(p = userdata);
    pa_assert_se(backend = p->context_data);
    pa_assert_se(r = dbus_pending_call_steal_reply(pending));

    if (!dbus_message_iter_init(r, &arg_i) || !pa_streq(dbus_message_get_signature(r), "a(oa{sv})")) {
        pa_log_error("Failed to parse " OFONO_VOICECALL_MANAGER_INTERFACE ".GetCalls");
        goto finish;
    }

    pa_assert(dbus_message_iter_get_arg_type(&arg_i) == DBUS_TYPE_ARRAY);
    dbus_message_iter_recurse(&arg_i, &array_i);

    while (dbus_message_iter_get_arg_type(&array_i) == DBUS_TYPE_STRUCT) {
        dbus_message_iter_recurse(&array_i, &struct_i);
        pa_assert(dbus_message_iter_get_arg_type(&struct_i) == DBUS_TYPE_OBJECT_PATH);
        voicecall_parse_call(backend, &struct_i);
        dbus_message_iter_next(&array_i);
    }

finish:
    dbus_message_unref(r);

    PA_LLIST_REMOVE(pa_dbus_pending, backend->pending, p);
    pa_dbus_pending_free(p);
}

static void voicecall_get_calls(pa_bluetooth_backend *backend, const char *modem_path) {
    DBusMessage *m;

    pa_assert(backend);
    pa_assert_se(m = dbus_message_new_method_call(OFONO_SERVICE, modem_path, OFONO_VOICECALL_MANAGER_INTERFACE, "GetCalls"));
    send_and_add_to_pending(backend, m, get_calls_reply, NULL);
}

static void get_modems_reply(DBusPendingCall *pending, void *userdata) {
    DBusMessage *r;
    pa_dbus_pending *p;
    pa_bluetooth_backend *backend;
    DBusMessageIter arg_i, array_i, struct_i;

    pa_assert(pending);
    pa_assert_se(p = userdata);
    pa_assert_se(backend = p->context_data);
    pa_assert_se(r = dbus_pending_call_steal_reply(pending));

    if (dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR) {
        pa_log_error(OFONO_MANAGER_INTERFACE ".GetModems() failed: %s: %s", dbus_message_get_error_name(r),
                     pa_dbus_get_error_message(r));
        goto finish;
    }

    if (!dbus_message_iter_init(r, &arg_i) || !pa_streq(dbus_message_get_signature(r), "a(oa{sv})")) {
        pa_log_error("Failed to parse " OFONO_MANAGER_INTERFACE ".GetModems");
        goto finish;
    }

    pa_assert(dbus_message_iter_get_arg_type(&arg_i) == DBUS_TYPE_ARRAY);
    dbus_message_iter_recurse(&arg_i, &array_i);

    while (dbus_message_iter_get_arg_type(&array_i) == DBUS_TYPE_STRUCT) {
        const char *modem_path;
        dbus_message_iter_recurse(&array_i, &struct_i);
        pa_assert(dbus_message_iter_get_arg_type(&struct_i) == DBUS_TYPE_OBJECT_PATH);
        dbus_message_iter_get_basic(&struct_i, &modem_path);
        voicecall_get_calls(backend, modem_path);
        dbus_message_iter_next(&array_i);
    }

finish:
    dbus_message_unref(r);

    PA_LLIST_REMOVE(pa_dbus_pending, backend->pending, p);
    pa_dbus_pending_free(p);
}

static void voicecall_get_all_calls(pa_bluetooth_backend *backend) {
    DBusMessage *m;

    pa_assert(backend);
    pa_assert_se(m = dbus_message_new_method_call(OFONO_SERVICE, "/", OFONO_MANAGER_INTERFACE, "GetModems"));
    send_and_add_to_pending(backend, m, get_modems_reply, NULL);
}

static void voicecall_clear_calls(pa_bluetooth_backend *backend) {
    pa_assert(backend);

    pa_hashmap_remove_all(backend->active_calls);
    pa_hashmap_remove_all(backend->held_calls);
    pa_hashmap_remove_all(backend->call_paths);
    backend->incoming_call_path = NULL;
}

static DBusHandlerResult filter_cb(DBusConnection *bus, DBusMessage *m, void *data) {
    DBusError err;
    pa_bluetooth_backend *backend = data;

    pa_assert(bus);
    pa_assert(m);
    pa_assert(backend);

    if (!backend->trfc)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    dbus_error_init(&err);

    if (dbus_message_is_signal(m, "org.freedesktop.DBus", "NameOwnerChanged")) {
        const char *name, *old_owner, *new_owner;

        if (!dbus_message_get_args(m, &err,
                                   DBUS_TYPE_STRING, &name,
                                   DBUS_TYPE_STRING, &old_owner,
                                   DBUS_TYPE_STRING, &new_owner,
                                   DBUS_TYPE_INVALID)) {
            pa_log_error("Failed to parse org.freedesktop.DBus.NameOwnerChanged: %s", err.message);
            goto fail;
        }

        if (pa_streq(name, OFONO_SERVICE)) {

            if (old_owner && *old_owner) {
                pa_log_debug("oFono disappeared");
                voicecall_clear_calls(backend);
            }

            if (new_owner && *new_owner) {
                pa_log_debug("oFono appeared");
            }
        }

    } else if (dbus_message_is_signal(m, OFONO_VOICECALL_INTERFACE, "PropertyChanged")) {
        const char *path;
        const char *property;
        const char *state;
        DBusMessageIter arg_i, var_i;

        if (!dbus_message_iter_init(m, &arg_i) || !pa_streq(dbus_message_get_signature(m), "sv")) {
            pa_log_error("Failed to parse " OFONO_VOICECALL_INTERFACE ".PropertyChanged");
            goto fail;
        }

        path = dbus_message_get_path(m);
        dbus_message_iter_get_basic(&arg_i, &property);

        pa_assert_se(dbus_message_iter_next(&arg_i));
        pa_assert(dbus_message_iter_get_arg_type(&arg_i) == DBUS_TYPE_VARIANT);

        if (pa_streq(property, "State")) {
            dbus_message_iter_recurse(&arg_i, &var_i);
            pa_assert(dbus_message_iter_get_arg_type(&var_i) == DBUS_TYPE_STRING);
            dbus_message_iter_get_basic(&var_i, &state);
            pa_log_debug("PropertyChanged %s: %s %s", path, property, state);
            if (pa_streq(state, "active")) {
                char *p;
                if (pa_safe_streq(backend->incoming_call_path, path))
                    backend->incoming_call_path = NULL;
                pa_hashmap_remove(backend->held_calls, path);
                pa_hashmap_remove(backend->active_calls, path);
                if ((p = pa_hashmap_get(backend->call_paths, path)))
                    pa_hashmap_put(backend->active_calls, p, p);
            } else if (pa_streq(state, "held")) {
                char *p;
                pa_hashmap_remove(backend->active_calls, path);
                if ((p = pa_hashmap_get(backend->call_paths, path)))
                    pa_hashmap_put(backend->held_calls, p, p);
            } else if (pa_streq(state, "disconnected")) {
                if (pa_safe_streq(backend->incoming_call_path, path))
                    backend->incoming_call_path = NULL;
                pa_hashmap_remove(backend->active_calls, path);
                pa_hashmap_remove(backend->held_calls, path);
                pa_hashmap_remove(backend->call_paths, path);
            }
            if (backend->trfc)
                rfcomm_ring_stop(backend->trfc);
        }

    } else if (dbus_message_is_signal(m, OFONO_VOICECALL_MANAGER_INTERFACE, "CallAdded")) {
        DBusMessageIter arg_i;

        if (!dbus_message_iter_init(m, &arg_i) || !pa_streq(dbus_message_get_signature(m), "oa{sv}")) {
            pa_log_error("Failed to parse " OFONO_VOICECALL_MANAGER_INTERFACE ".CallAdded");
            goto fail;
        }

        pa_assert(dbus_message_iter_get_arg_type(&arg_i) == DBUS_TYPE_OBJECT_PATH);
        voicecall_parse_call(backend, &arg_i);
    }

fail:
    dbus_error_free(&err);
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static bool profile_init(pa_bluetooth_backend *b, pa_bluetooth_profile_t profile) {
    static const DBusObjectPathVTable vtable_profile = {
        .message_function = profile_handler,
    };
    const char *object_name;
    const char *uuid;
    DBusError err;

    pa_assert(b);

    dbus_error_init(&err);

    if (!dbus_connection_add_filter(pa_dbus_connection_get(b->connection), filter_cb, b, NULL)) {
        pa_log_error("Failed to add HSP filter function");
        return false;
    }

    if (pa_dbus_add_matches(pa_dbus_connection_get(b->connection), &err,
            "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged',"
            "arg0='" OFONO_SERVICE "'",
            "type='signal',sender='" OFONO_SERVICE "',interface='" OFONO_VOICECALL_INTERFACE "',member='PropertyChanged'",
            "type='signal',sender='" OFONO_SERVICE "',interface='" OFONO_VOICECALL_MANAGER_INTERFACE "',member='CallAdded'",
            NULL) < 0) {
        pa_log("Failed to add HSP oFono D-Bus matches: %s", err.message);
        dbus_connection_remove_filter(pa_dbus_connection_get(b->connection), filter_cb, b);
        dbus_error_free(&err);
        return false;
    }

    switch (profile) {
        case PA_BLUETOOTH_PROFILE_DROID_HEADSET_HSP:
            object_name = HSP_AG_PROFILE;
            uuid = PA_BLUETOOTH_UUID_HSP_AG;
            break;
        default:
            pa_assert_not_reached();
            break;
    }

    pa_assert_se(dbus_connection_register_object_path(pa_dbus_connection_get(b->connection), object_name, &vtable_profile, b));
    register_profile(b, object_name, uuid);

    return true;
}

static void profile_done(pa_bluetooth_backend *b, pa_bluetooth_profile_t profile) {
    pa_assert(b);

    pa_dbus_remove_matches(pa_dbus_connection_get(b->connection),
            "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged',"
            "arg0='" OFONO_SERVICE "'",
            "type='signal',sender='" OFONO_SERVICE "',interface='" OFONO_VOICECALL_INTERFACE "',member='PropertyChanged'",
            "type='signal',sender='" OFONO_SERVICE "',interface='" OFONO_VOICECALL_MANAGER_INTERFACE "',member='CallAdded'",
            NULL);
    dbus_connection_remove_filter(pa_dbus_connection_get(b->connection), filter_cb, b);

    switch (profile) {
        case PA_BLUETOOTH_PROFILE_DROID_HEADSET_HSP:
            dbus_connection_unregister_object_path(pa_dbus_connection_get(b->connection), HSP_AG_PROFILE);
            break;
        default:
            pa_assert_not_reached();
            break;
    }
}

pa_bluetooth_backend *pa_bluetooth_droid_backend_hsp_new(pa_core *c, pa_bluetooth_discovery *y, pa_droid_volume_control *volume) {
    pa_bluetooth_backend *backend;
    DBusError err;

    pa_log_debug("Bluetooth Headset Backend API support using the native backend");

    backend = pa_xnew0(pa_bluetooth_backend, 1);
    backend->core = c;
    backend->volume_control = volume;
    backend->call_paths = pa_hashmap_new_full(pa_idxset_string_hash_func,
                                              pa_idxset_string_compare_func,
                                              pa_xfree,
                                              NULL);
    backend->active_calls = pa_hashmap_new_full(pa_idxset_string_hash_func,
                                                pa_idxset_string_compare_func,
                                                NULL,
                                                NULL);
    backend->held_calls = pa_hashmap_new_full(pa_idxset_string_hash_func,
                                              pa_idxset_string_compare_func,
                                              NULL,
                                              NULL);

    dbus_error_init(&err);
    if (!(backend->connection = pa_dbus_bus_get(c, DBUS_BUS_SYSTEM, &err))) {
        pa_log("Failed to get D-Bus connection: %s", err.message);
        dbus_error_free(&err);
        pa_xfree(backend);
        return NULL;
    }

    backend->discovery = y;

    if (!profile_init(backend, PA_BLUETOOTH_PROFILE_DROID_HEADSET_HSP)) {
        pa_dbus_connection_unref(backend->connection);
        pa_xfree(backend);
        return NULL;
    }

    return backend;
}

void pa_bluetooth_droid_backend_hsp_free(pa_bluetooth_backend *backend) {
    pa_assert(backend);

    pa_hashmap_free(backend->active_calls);
    pa_hashmap_free(backend->held_calls);
    pa_hashmap_free(backend->call_paths);

    pa_dbus_free_pending_list(&backend->pending);

    profile_done(backend, PA_BLUETOOTH_PROFILE_DROID_HEADSET_HSP);

    pa_dbus_connection_unref(backend->connection);

    pa_xfree(backend);
}
