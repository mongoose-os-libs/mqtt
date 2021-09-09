/*
 * Copyright (c) 2019 Deomid "rojer" Ryabkov
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

#include <stdlib.h>

#include "mgos.h"
#include "mgos_debug.h"
#include "mgos_mongoose.h"

static void mgos_mqtt_conn_net_ev(int ev, void *evd, void *arg);
static void mgos_mqtt_free_cfg(struct mgos_config_mqtt *cfg);

static struct mgos_config_mqtt *mgos_mqtt_copy_cfg(
    const struct mgos_config_mqtt *cfg) {
  if (cfg == NULL) return NULL;
  struct mgos_config_mqtt *new_cfg = NULL;
  if (cfg == mgos_sys_config_get_mqtt() || cfg == mgos_sys_config_get_mqtt1()) {
    new_cfg = (struct mgos_config_mqtt *) cfg;
  } else {
    new_cfg = (struct mgos_config_mqtt *) calloc(1, sizeof(*new_cfg));
    if (new_cfg == NULL || !mgos_config_mqtt_copy(cfg, new_cfg)) {
      mgos_mqtt_free_cfg(new_cfg);
      free(new_cfg);
      new_cfg = NULL;
    } else {
    }
  }
  return new_cfg;
}

static void mgos_mqtt_free_cfg(struct mgos_config_mqtt *cfg) {
  if (cfg == NULL) return;
  if (cfg == mgos_sys_config_get_mqtt() || cfg == mgos_sys_config_get_mqtt1())
    return;
  mgos_config_mqtt_free(cfg);
  free(cfg);
}

struct mgos_mqtt_conn *mgos_mqtt_conn_create2(
    int conn_id, const struct mgos_config_mqtt *cfg0,
    const struct mgos_config_mqtt *cfg1) {
  struct mgos_mqtt_conn *c = (struct mgos_mqtt_conn *) calloc(1, sizeof(*c));
  if (c == NULL) return NULL;
  c->conn_id = (int16_t) conn_id;
  c->cfg0 = mgos_mqtt_copy_cfg(cfg0);
  c->cfg1 = mgos_mqtt_copy_cfg(cfg1);
  c->cfg = c->cfg0;
  c->reconnect_timer_id = MGOS_INVALID_TIMER_ID;
  c->max_qos = -1;
  STAILQ_INIT(&c->queue);
  mgos_event_add_handler(MGOS_NET_EV_IP_ACQUIRED, mgos_mqtt_conn_net_ev, c);
  return c;
}

struct mgos_mqtt_conn *mgos_mqtt_conn_create(
    int conn_id, const struct mgos_config_mqtt *cfg) {
  return mgos_mqtt_conn_create2(conn_id, cfg, NULL);
}

struct mgos_mqtt_conn *mgos_mqtt_conn_from_nc(struct mg_connection *nc) {
  return (nc != NULL ? (struct mgos_mqtt_conn *) nc->user_data : NULL);
}

struct mg_connection *mgos_mqtt_conn_nc(struct mgos_mqtt_conn *c) {
  return (c != NULL ? c->nc : NULL);
}

static int adjust_qos(const struct mgos_mqtt_conn *c, int qos) {
  int max_qos = c->max_qos;
  if (max_qos < 0 && c->cfg != NULL) max_qos = c->cfg->max_qos;
  if (max_qos < 0) return qos;
  return MIN(qos, max_qos);
}

uint16_t mgos_mqtt_conn_get_packet_id(struct mgos_mqtt_conn *c) {
  while (true) {
    c->packet_id++;
    if (c->packet_id == 0) continue;
    struct mgos_mqtt_conn_queue_entry *qe = NULL;
    STAILQ_FOREACH(qe, &c->queue, next) {
      if (qe->packet_id == c->packet_id) break;
    }
    if (qe == NULL) break;
  }
  return c->packet_id;
}

static void mgos_mqtt_conn_queue_entry_free(
    struct mgos_mqtt_conn_queue_entry *qe) {
  free(qe->topic);
  mg_strfree(&qe->msg);
  free(qe);
}

static void mgos_mqtt_conn_queue_remove(struct mgos_mqtt_conn *c,
                                        uint16_t packet_id) {
  struct mgos_mqtt_conn_queue_entry *qe, *qet;
  STAILQ_FOREACH_SAFE(qe, &c->queue, next, qet) {
    if (qe->packet_id != packet_id) continue;
    STAILQ_REMOVE(&c->queue, qe, mgos_mqtt_conn_queue_entry, next);
    LOG(LL_DEBUG, ("MQTT%d ack %d", c->conn_id, qe->packet_id));
    mgos_mqtt_conn_queue_entry_free(qe);
  }
}

static bool call_topic_handler(struct mgos_mqtt_conn *c, int ev,
                               void *ev_data) {
  bool handled = false;
  struct mg_mqtt_message *msg = (struct mg_mqtt_message *) ev_data;
  struct mgos_mqtt_subscription *s;
  SLIST_FOREACH(s, &c->subscriptions, next) {
    if ((ev == MG_EV_MQTT_SUBACK && s->sub_id == msg->message_id) ||
        mg_mqtt_match_topic_expression(s->topic, msg->topic)) {
      if (!handled && ev == MG_EV_MQTT_PUBLISH && msg->qos > 0) {
        mg_mqtt_puback(c->nc, msg->message_id);
      }
      s->handler(c->nc, ev, ev_data, s->user_data);
      handled = true;
    }
  }
  return handled;
}

static void call_conn_handlers(struct mgos_mqtt_conn *c, int ev,
                               void *ev_data) {
  struct mgos_mqtt_conn_handler *ch;
  SLIST_FOREACH(ch, &c->conn_handlers, next) {
    ch->handler(c->nc, ev, ev_data, ch->user_data);
  }
}

static void do_pub(struct mgos_mqtt_conn *c, uint16_t packet_id,
                   const char *topic, struct mg_str msg, int flags) {
  int qos = MG_MQTT_GET_QOS(flags);
  bool dup = ((flags & MG_MQTT_DUP) != 0);
  bool retain = ((flags & MG_MQTT_RETAIN) != 0);
  LOG(LL_DEBUG,
      ("MQTT%d pub -> %d %s @ %d%s%s (%d): [%.*s]", c->conn_id, packet_id,
       topic, qos, (dup ? " DUP" : ""), (retain ? " RETAIN" : ""),
       (int) msg.len, (int) msg.len, msg.p));
  mg_mqtt_publish(c->nc, topic, packet_id, flags, msg.p, msg.len);
  (void) qos;
  (void) dup;
  (void) retain;
}

static void do_sub(struct mgos_mqtt_conn *c, struct mgos_mqtt_subscription *s) {
  struct mg_mqtt_topic_expression te = {.topic = s->topic.p,
                                        .qos = adjust_qos(c, s->qos)};
  s->sub_id = mgos_mqtt_conn_get_packet_id(c);
  mg_mqtt_subscribe(c->nc, &te, 1, s->sub_id);
  LOG(LL_INFO, ("MQTT%d sub %s @ %d", c->conn_id, te.topic, te.qos));
}

void mgos_mqtt_ev(struct mg_connection *nc, int ev, void *ev_data,
                  void *user_data) {
  struct mgos_mqtt_conn *c = (struct mgos_mqtt_conn *) user_data;
  if (c == NULL || nc != c->nc) {
    nc->flags |= MG_F_CLOSE_IMMEDIATELY;
    return;
  }
  if (ev > MG_MQTT_EVENT_BASE) {
    LOG(LL_DEBUG, ("MQTT%d event: %d", c->conn_id, ev));
  }
  const struct mgos_config_mqtt *cfg = c->cfg;
  const struct mg_mqtt_message *msg = (struct mg_mqtt_message *) ev_data;
  switch (ev) {
    case MG_EV_CONNECT: {
      int status = *((int *) ev_data);
      LOG(LL_INFO, ("MQTT%d %s connect %s (%d)", c->conn_id,
                    (cfg->ws_enable ? "WS" : "TCP"),
                    (status == 0 ? "ok" : "error"), status));
      if (status != 0) break;
      struct mg_send_mqtt_handshake_opts opts;
      memset(&opts, 0, sizeof(opts));
      opts.user_name = cfg->user;
      opts.password = cfg->pass;
      if (cfg->clean_session) {
        opts.flags |= MG_MQTT_CLEAN_SESSION;
      }
      opts.keep_alive = cfg->keep_alive;
      opts.will_topic = cfg->will_topic;
      opts.will_message = cfg->will_message;
      if (cfg->will_retain) {
        opts.flags |= MG_MQTT_WILL_RETAIN;
      }
      const char *client_id =
          (cfg->client_id != NULL ? cfg->client_id
                                  : mgos_sys_config_get_device_id());
      if (c->connect_fn != NULL) {
        c->connect_fn(nc, client_id, &opts, c->connect_fn_arg);
      } else {
        mg_send_mqtt_handshake_opt(nc, client_id, opts);
      }
      break;
    }
    case MG_EV_CLOSE: {
      LOG(LL_INFO, ("MQTT%d Disconnect", c->conn_id));
      c->nc = NULL;
      bool connected = c->connected;
      c->connected = false;
      if (connected) {
        if (cfg->cloud_events) {
          struct mgos_cloud_arg arg = {.type = MGOS_CLOUD_MQTT};
          mgos_event_trigger(MGOS_EVENT_CLOUD_DISCONNECTED, &arg);
        }
        call_conn_handlers(c, ev, ev_data);
      }
      mgos_mqtt_conn_reconnect(c);
      break;
    }
    case MG_EV_POLL: {
      call_conn_handlers(c, ev, ev_data);
      break;
    }
    case MG_EV_MQTT_CONNACK: {
      int code = ((struct mg_mqtt_message *) ev_data)->connack_ret_code;
      LOG((code == 0 ? LL_INFO : LL_ERROR),
          ("MQTT%d CONNACK %d", c->conn_id, code));
      if (code == 0) {
        c->connected = true;
        c->reconnect_timeout_ms = 0;
        if (cfg->cloud_events) {
          struct mgos_cloud_arg arg = {.type = MGOS_CLOUD_MQTT};
          mgos_event_trigger(MGOS_EVENT_CLOUD_CONNECTED, &arg);
        }
        struct mgos_mqtt_subscription *s;
        SLIST_FOREACH(s, &c->subscriptions, next) {
          do_sub(c, s);
        }
        if (!STAILQ_EMPTY(&c->queue)) {
          struct mgos_mqtt_conn_queue_entry *qe;
          STAILQ_FOREACH(qe, &c->queue, next) {
            qe->flags &= ~MG_MQTT_DUP;
          }
          c->queue_drained = false;
        } else {
          c->queue_drained = true;
        }
        call_conn_handlers(c, ev, ev_data);
      } else {
        nc->flags |= MG_F_CLOSE_IMMEDIATELY;
      }
      break;
    }
    /* Delegate almost all MQTT events to the user's handler */
    case MG_EV_MQTT_SUBACK:
    case MG_EV_MQTT_PUBLISH:
      if (call_topic_handler(c, ev, ev_data)) break;
      call_conn_handlers(c, ev, ev_data);
      break;
    case MG_EV_MQTT_PUBACK:
      mgos_mqtt_conn_queue_remove(c, msg->message_id);
      /* fall through */
    case MG_EV_MQTT_CONNECT:
    case MG_EV_MQTT_PUBREL:
    case MG_EV_MQTT_PUBCOMP:
    case MG_EV_MQTT_SUBSCRIBE:
    case MG_EV_MQTT_UNSUBSCRIBE:
    case MG_EV_MQTT_UNSUBACK:
    case MG_EV_MQTT_PINGREQ:
    case MG_EV_MQTT_DISCONNECT:
      call_conn_handlers(c, ev, ev_data);
      break;
    case MG_EV_MQTT_PUBREC: {
      mg_mqtt_pubrel(nc, msg->message_id);
      call_conn_handlers(c, ev, ev_data);
      mgos_mqtt_conn_queue_remove(c, msg->message_id);
      break;
    }
  }
  /* If we have received some sort of MQTT message, queue is not empty
   * and we have nothing else to send, drain the queue.
   * This will usually be CONNACK and PUBACK, but can be PINGRESP as well. */
  if ((ev == MG_EV_MQTT_CONNACK || ev == MG_EV_MQTT_PUBACK ||
       ev == MG_EV_MQTT_PUBREC || ev == MG_EV_MQTT_PINGRESP) &&
      c->connected) {
    struct mgos_mqtt_conn_queue_entry *qe;
    if (ev == MG_EV_MQTT_PINGRESP) {
      /* PINGRESP allows re-retransmission, i.e. we will re-send messages
       * we have already sent in this connection and that server has not yet
       * acked for some reason. This rarely (if ever) happens. */
      STAILQ_FOREACH(qe, &c->queue, next) {
        qe->flags &= ~MG_MQTT_DUP;
      }
    }
    if (!STAILQ_EMPTY(&c->queue)) {
      STAILQ_FOREACH(qe, &c->queue, next) {
        if (!(qe->flags & MG_MQTT_DUP)) break;
      }
      if (qe != NULL) {
        qe->flags |= MG_MQTT_DUP;
        do_pub(c, qe->packet_id, qe->topic, qe->msg, qe->flags);
      }
    } else if (!c->queue_drained && STAILQ_EMPTY(&c->queue)) {
      // The queue has been drained at least once, immediate publishing is
      // allowed.
      c->queue_drained = true;
      LOG(LL_DEBUG, ("MQTT%d queue drained", c->conn_id));
    }
  }
}

void mgos_mqtt_ws_ev(struct mg_connection *nc, int ev, void *ev_data,
                     void *user_data);

void mgos_mqtt_conn_add_handler(struct mgos_mqtt_conn *c,
                                mg_event_handler_t handler, void *user_data) {
  if (c == NULL) return;
  struct mgos_mqtt_conn_handler *ch = calloc(1, sizeof(*ch));
  ch->handler = handler;
  ch->user_data = user_data;
  SLIST_INSERT_HEAD(&c->conn_handlers, ch, next);
}

void mgos_mqtt_conn_set_connect_fn(struct mgos_mqtt_conn *c,
                                   mgos_mqtt_connect_fn_t fn, void *fn_arg) {
  if (c == NULL) return;
  c->connect_fn = fn;
  c->connect_fn_arg = fn_arg;
}

static void mgos_mqtt_conn_net_ev(int ev, void *evd, void *arg) {
  struct mgos_mqtt_conn *c = arg;
  if (c->reconnect_timeout_ms < 0) {
    return;  // User doesn't want us to reconnect.
  }
  mgos_mqtt_conn_connect(c);
  (void) evd;
  (void) ev;
}

void mgos_mqtt_conn_set_max_qos(struct mgos_mqtt_conn *c, int qos) {
  if (c == NULL || c->cfg == NULL || c->cfg->max_qos == qos) return;
  c->max_qos = (int16_t) qos;
}

bool mgos_mqtt_conn_set_config(struct mgos_mqtt_conn *c,
                               const struct mgos_config_mqtt *cfg) {
  if (c == NULL) return false;
  bool ret = false;
  struct mgos_config_mqtt *new_cfg = NULL;
  if (!cfg->enable) {
    ret = true;
    goto out;
  }
  if (cfg->server == NULL) {
    LOG(LL_ERROR, ("MQTT requires server name"));
    goto out;
  }
  new_cfg = mgos_mqtt_copy_cfg(cfg);
  ret = (new_cfg != NULL);

out:
  if (ret) {
    const struct mgos_config_mqtt *old_cfg = c->cfg;
    c->cfg = new_cfg;
    if (c->nc != NULL) {
      c->nc->flags |= MG_F_CLOSE_IMMEDIATELY;
      c->nc = NULL;
    }
    mgos_mqtt_free_cfg((struct mgos_config_mqtt *) old_cfg);
  } else {
    mgos_mqtt_free_cfg(new_cfg);
  }
  return ret;
}

static bool mgos_mqtt_time_ok(struct mgos_mqtt_conn *c) {
  if (c->cfg == NULL) return false;
  if (!c->cfg->require_time) return true;
  if (mg_time() < 1500000000) {
    LOG(LL_ERROR, ("Time is not set, not connecting"));
    return false;
  }
  return true;
}

bool mgos_mqtt_conn_connect(struct mgos_mqtt_conn *c) {
  bool ret = true;
  char *server = NULL;
  const char *err_str = NULL;
  struct mg_connect_opts opts = {0};

  if (c == NULL || c->cfg == NULL || !c->cfg->enable ||
      c->cfg->server == NULL) {
    return false;
  }

  /* If we're already connected, do nothing */
  if (c->nc != NULL) return true;

  if (c->reconnect_timeout_ms < 0) {
    c->reconnect_timeout_ms = 0;
  }

  if (!mgos_mqtt_time_ok(c)) {
    mgos_mqtt_conn_reconnect(c);
    return false;
  }

  const struct mgos_config_mqtt *cfg = c->cfg;
  opts.error_string = &err_str;
#if MG_ENABLE_SSL
  opts.ssl_cert = cfg->ssl_cert;
  opts.ssl_key = cfg->ssl_key;
  opts.ssl_ca_cert = cfg->ssl_ca_cert;
  opts.ssl_cipher_suites = cfg->ssl_cipher_suites;
  opts.ssl_psk_identity = cfg->ssl_psk_identity;
  opts.ssl_psk_key = cfg->ssl_psk_key;
#endif
  if (strchr(cfg->server, ':') == NULL) {
    int port = (cfg->ssl_ca_cert != NULL ? 8883 : 1883);
    mg_asprintf(&server, 0, "%s:%d", cfg->server, port);
    if (server == NULL) return false;
  } else {
    server = strdup(cfg->server);
  }
  LOG(LL_INFO, ("MQTT%d connecting to %s", c->conn_id, server));
  struct mgos_cloud_arg arg = {.type = MGOS_CLOUD_MQTT};
  mgos_event_trigger(MGOS_EVENT_CLOUD_CONNECTING, &arg);

  c->connected = false;
  mg_event_handler_t h = (cfg->ws_enable ? mgos_mqtt_ws_ev : mgos_mqtt_ev);
  c->nc = mg_connect_opt(mgos_get_mgr(), server, h, c, opts);
  if (c->nc != NULL) {
    if (cfg->ws_enable) {
      mg_set_protocol_http_websocket(c->nc);
    } else {
      mg_set_protocol_mqtt(c->nc);
    }
    c->nc->recv_mbuf_limit = cfg->recv_mbuf_limit;
  } else {
    LOG(LL_ERROR, ("Error: %s", err_str));
    mgos_mqtt_conn_reconnect(c);
    ret = false;
  }
  free(server);
  return ret;
}

void mgos_mqtt_conn_disconnect(struct mgos_mqtt_conn *c) {
  if (c == NULL) return;
  c->reconnect_timeout_ms = -1;  // Prevent reconnect.
  if (c->nc != NULL) {
    mg_mqtt_disconnect(c->nc);
    c->nc->flags |= MG_F_SEND_AND_CLOSE;
  }
}

bool mgos_mqtt_conn_is_connected(struct mgos_mqtt_conn *c) {
  return (c != NULL && c->connected);
}

static void reconnect_timer_cb(void *arg) {
  struct mgos_mqtt_conn *c = arg;
  c->reconnect_timer_id = MGOS_INVALID_TIMER_ID;
  mgos_mqtt_conn_connect(c);
}

static void mqtt_switch_config(struct mgos_mqtt_conn *c) {
  const struct mgos_config_mqtt *cfg;
  if (c->cfg == c->cfg0) {
    cfg = c->cfg1;
  } else if (c->cfg == c->cfg1) {
    cfg = c->cfg0;
  } else {
    /* User set a custom config - don't mess with it. */
    return;
  }
  if (cfg != NULL && cfg->enable) {
    c->cfg = cfg;
    c->reconnect_timeout_ms = cfg->reconnect_timeout_min * 1000;
  }
}

void mgos_mqtt_conn_reconnect(struct mgos_mqtt_conn *c) {
  int rt_ms;
  if (c == NULL || c->cfg == NULL || c->cfg->server == NULL ||
      c->reconnect_timeout_ms < 0) {
    return;
  }

  if (c->reconnect_timeout_ms >= c->cfg->reconnect_timeout_max * 1000) {
    mqtt_switch_config(c);
  }

  const struct mgos_config_mqtt *cfg = c->cfg;

  if (c->reconnect_timeout_ms <= 0) c->reconnect_timeout_ms = 1000;

  rt_ms = c->reconnect_timeout_ms * 2;

  if (rt_ms < cfg->reconnect_timeout_min * 1000) {
    rt_ms = cfg->reconnect_timeout_min * 1000;
  }
  if (rt_ms > cfg->reconnect_timeout_max * 1000) {
    rt_ms = cfg->reconnect_timeout_max * 1000;
  }
  c->reconnect_timeout_ms = rt_ms;
  /* Fuzz the time a little. */
  rt_ms = (int) mgos_rand_range(rt_ms * 0.9, rt_ms * 1.1);
  LOG(LL_INFO, ("MQTT%d connecting after %d ms", c->conn_id, rt_ms));
  mgos_clear_timer(c->reconnect_timer_id);
  c->reconnect_timer_id = mgos_set_timer(rt_ms, 0, reconnect_timer_cb, c);
}

uint16_t mgos_mqtt_conn_pub(struct mgos_mqtt_conn *c, const char *topic,
                            struct mg_str msg, int qos, bool retain) {
  if (c == NULL) return 0;
  bool published = false;
  uint16_t packet_id = mgos_mqtt_conn_get_packet_id(c);
  int flags = MG_MQTT_QOS(adjust_qos(c, qos));
  if (retain) flags |= MG_MQTT_RETAIN;
  // If we are connected and have drained the initial queue, publish immediately
  // On initial connection we want queue to be processed in order, without
  // later entries jumping ahead of the queue - this could cause out of order
  // shadow updates, for example, where a newer update would be overridden by
  // an entry replayed from the queue.
  if (c->connected && (qos == 0 || c->queue_drained)) {
    do_pub(c, packet_id, topic, msg, flags);
    published = true;
  }
  if (qos > 0 && c->cfg->max_queue_length > 0) {
    size_t qlen = 0;
    struct mgos_mqtt_conn_queue_entry *qe;
    STAILQ_FOREACH(qe, &c->queue, next) {
      qlen++;
    }
    while (qlen >= (size_t) c->cfg->max_queue_length) {
      LOG(LL_ERROR, ("MQTT%d queue overflow!", c->conn_id));
      qe = STAILQ_FIRST(&c->queue);
      STAILQ_REMOVE_HEAD(&c->queue, next);
      mgos_mqtt_conn_queue_entry_free(qe);
      qlen--;
    }
    qe = calloc(1, sizeof(*qe));
    if (qe == NULL) goto out;
    qe->topic = strdup(topic);
    qe->packet_id = packet_id;
    qe->flags = (uint16_t) flags;
    qe->msg = mg_strdup(msg);
    // Prevent immediate retry.
    if (published) qe->flags |= MG_MQTT_DUP;
    STAILQ_INSERT_TAIL(&c->queue, qe, next);
  }
out:
  return packet_id;
}

uint16_t mgos_mqtt_conn_pubv(struct mgos_mqtt_conn *c, const char *topic,
                             int qos, bool retain, const char *json_fmt,
                             va_list ap) {
  if (c == NULL) return 0;
  uint16_t res = 0;
  char *msg = json_vasprintf(json_fmt, ap);
  if (msg != NULL) {
    res = mgos_mqtt_conn_pub(c, topic, mg_mk_str(msg), qos, retain);
    free(msg);
  }
  return res;
}

uint16_t mgos_mqtt_conn_pubf(struct mgos_mqtt_conn *c, const char *topic,
                             int qos, bool retain, const char *json_fmt, ...) {
  if (c == NULL) return 0;
  uint16_t res;
  va_list ap;
  va_start(ap, json_fmt);
  res = mgos_mqtt_conn_pubv(c, topic, qos, retain, json_fmt, ap);
  va_end(ap);
  return res;
}

void mgos_mqtt_conn_sub_s(struct mgos_mqtt_conn *c, struct mg_str topic,
                          int qos, mg_event_handler_t handler,
                          void *user_data) {
  if (c == NULL) return;
  struct mgos_mqtt_subscription *s = calloc(1, sizeof(*s));
  s->topic = mg_strdup_nul(topic);
  s->handler = handler;
  s->user_data = user_data;
  s->qos = adjust_qos(c, qos);
  SLIST_INSERT_HEAD(&c->subscriptions, s, next);
  if (c->connected) do_sub(c, s);
}

void mgos_mqtt_conn_sub(struct mgos_mqtt_conn *c, const char *topic, int qos,
                        mg_event_handler_t handler, void *user_data) {
  mgos_mqtt_conn_sub_s(c, mg_mk_str(topic), qos, handler, user_data);
}

bool mgos_mqtt_conn_unsub(struct mgos_mqtt_conn *c, const char *topic) {
  struct mgos_mqtt_subscription *s;

  SLIST_FOREACH(s, &c->subscriptions, next) {
    if (0 == mg_vcmp(&s->topic, topic)) {
      LOG(LL_INFO,
          ("MQTT%d unsub %.*s", c->conn_id, (int) s->topic.len, s->topic.p));
      if (c->connected) {
        mg_mqtt_unsubscribe(c->nc, (char **) &topic, 1,
                            mgos_mqtt_conn_get_packet_id(c));
      }
      SLIST_REMOVE(&c->subscriptions, s, mgos_mqtt_subscription, next);
      mg_strfree(&s->topic);
      free(s->user_data);
      free(s);
      return true;
    }
  }
  return false;
}

size_t mgos_mqtt_conn_num_unsent_bytes(struct mgos_mqtt_conn *c) {
  size_t num_bytes = 0;
  if (c == NULL) return 0;
  struct mgos_mqtt_conn_queue_entry *qe;
  STAILQ_FOREACH(qe, &c->queue, next) {
    num_bytes += qe->msg.len;
  }
  if (c->nc != NULL) num_bytes += c->nc->send_mbuf.len;
  return num_bytes;
}
