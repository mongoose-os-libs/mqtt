/*
 * Copyright (c) 2021 Deomid "rojer" Ryabkov
 * All rights reserved
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "mgos_mqtt_conn_internal.h"

#include "mgos.h"
#include "mgos_mongoose.h"

/*
 * This is a WebSocket wrapper for an MQTT connection.
 * The outer connection handler is WS that invokes the MQTT handler.
 * It removes framing on the way in and adds on the way out.
 * State of the MQTT handler is maintained separately and is swapped in and out
 * before and after invocation.
 */

typedef void (*mg_proto_data_destructor_t)(void *);

struct mgos_mqtt_ws_data {
  mg_event_handler_t mqtt_proto_handler, ws_proto_handler;
  void *mqtt_proto_data, *ws_proto_data;
  mg_proto_data_destructor_t mqtt_proto_data_destructor,
      ws_proto_data_destructor;
  struct mbuf mqtt_recv_mbuf, ws_recv_mbuf;
  struct mbuf mqtt_send_mbuf, ws_send_mbuf;
};

static void ws_to_mqtt(struct mg_connection *nc,
                       struct mgos_mqtt_ws_data *wsd) {
  mbuf_move(&nc->recv_mbuf, &wsd->ws_recv_mbuf);
  mbuf_move(&nc->send_mbuf, &wsd->ws_send_mbuf);
  mbuf_move(&wsd->mqtt_recv_mbuf, &nc->recv_mbuf);
  mbuf_move(&wsd->mqtt_send_mbuf, &nc->send_mbuf);
  nc->proto_handler = wsd->mqtt_proto_handler;
  nc->proto_data = wsd->mqtt_proto_data;
  nc->proto_data_destructor = wsd->mqtt_proto_data_destructor;
  nc->handler = mgos_mqtt_ev;
}

static void mqtt_to_ws(struct mg_connection *nc,
                       struct mgos_mqtt_ws_data *wsd) {
  mbuf_move(&nc->recv_mbuf, &wsd->mqtt_recv_mbuf);
  mbuf_move(&nc->send_mbuf, &wsd->mqtt_send_mbuf);
  mbuf_move(&wsd->ws_recv_mbuf, &nc->recv_mbuf);
  mbuf_move(&wsd->ws_send_mbuf, &nc->send_mbuf);
  nc->proto_handler = wsd->ws_proto_handler;
  nc->proto_data = wsd->ws_proto_data;
  nc->proto_data_destructor = wsd->ws_proto_data_destructor;
  nc->handler = mgos_mqtt_ws_ev;
  if (wsd->mqtt_send_mbuf.len > 0) {
    mg_send_websocket_frame(nc, WEBSOCKET_OP_BINARY, wsd->mqtt_send_mbuf.buf,
                            wsd->mqtt_send_mbuf.len);
    mbuf_clear(&wsd->mqtt_send_mbuf);
  }
}

static void mgos_mqtt_ws_data_free(struct mgos_mqtt_ws_data *wsd) {
  if (wsd == NULL) return;
  mbuf_free(&wsd->mqtt_recv_mbuf);
  mbuf_free(&wsd->mqtt_send_mbuf);
  mbuf_free(&wsd->ws_recv_mbuf);
  mbuf_free(&wsd->ws_send_mbuf);
  if (wsd->mqtt_proto_data_destructor != NULL && wsd->ws_proto_data != NULL) {
    wsd->mqtt_proto_data_destructor(wsd->mqtt_proto_data);
  }
  free(wsd);
}

void mgos_mqtt_ws_ev(struct mg_connection *nc, int ev, void *ev_data,
                     void *user_data) {
  struct mgos_mqtt_conn *c = (struct mgos_mqtt_conn *) user_data;
  if (c == NULL || nc != c->nc) {
    nc->flags |= MG_F_CLOSE_IMMEDIATELY;
    return;
  }
  if (ev >= MG_EV_WEBSOCKET_HANDSHAKE_REQUEST &&
      ev <= MG_EV_WEBSOCKET_CONTROL_FRAME) {
    LOG(LL_DEBUG, ("MQTT%d WS event: %d", c->conn_id, ev));
  }
  const struct mgos_config_mqtt *cfg = c->cfg;
  struct mgos_mqtt_ws_data *wsd = c->ws_data;
  switch (ev) {
    case MG_EV_CONNECT: {
      int status = *((int *) ev_data);
      LOG(LL_INFO, ("MQTT%d %s connect %s (%d)", c->conn_id, "TCP",
                    (status == 0 ? "ok" : "error"), status));
      if (status != 0) break;
      LOG(LL_INFO,
          ("MQTT%d sending WS handshake to %s", c->conn_id, cfg->ws_path));
      mg_send_websocket_handshake2(nc, cfg->ws_path, cfg->server, "mqtt", NULL);
      break;
    }
    case MG_EV_WEBSOCKET_HANDSHAKE_DONE: {
      wsd = calloc(1, sizeof(*wsd));
      if (wsd == NULL) {
        nc->flags |= MG_F_CLOSE_IMMEDIATELY;
        break;
      }
      wsd->ws_proto_handler = nc->proto_handler;
      wsd->ws_proto_data = nc->proto_data;
      wsd->ws_proto_data_destructor = nc->proto_data_destructor;
      c->ws_data = wsd;
      // Emulate the "connect" event for the MQTT handler.
      int status = 0;
      mbuf_clear(&nc->recv_mbuf);
      ws_to_mqtt(nc, wsd);
      mg_set_protocol_mqtt(nc);
      wsd->mqtt_proto_handler = nc->proto_handler;
      wsd->mqtt_proto_data = nc->proto_data;
      wsd->mqtt_proto_data_destructor = nc->proto_data_destructor;
      nc->proto_handler(nc, MG_EV_CONNECT, &status, user_data);
      mqtt_to_ws(nc, wsd);
      break;
    }
    case MG_EV_WEBSOCKET_FRAME: {
      // Emulate the "receive" event.
      struct websocket_message *wm = ev_data;
      mbuf_append(&wsd->mqtt_recv_mbuf, wm->data, wm->size);
      wsd->ws_proto_handler = nc->proto_handler;
      ws_to_mqtt(nc, wsd);
      int size = (int) wm->size;
      nc->proto_handler(nc, MG_EV_RECV, &size, user_data);
      mqtt_to_ws(nc, wsd);
      break;
    }
    case MG_EV_POLL:
    case MG_EV_CLOSE: {
      bool was_connected = c->connected;
      if (was_connected) {
        ws_to_mqtt(nc, wsd);
        nc->proto_handler(nc, ev, NULL, user_data);
        mqtt_to_ws(nc, wsd);
      }
      if (ev == MG_EV_CLOSE) {
        c->ws_data = NULL;
        mgos_mqtt_ws_data_free(wsd);
        if (!was_connected) {
          // Do the part normally done by mgos_mqtt_ev.
          c->nc = NULL;
          mgos_mqtt_conn_reconnect(c);
        }
      }
      break;
    }
  }
}
