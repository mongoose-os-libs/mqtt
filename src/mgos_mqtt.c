/*
 * Copyright (c) 2014-2018 Cesanta Software Limited
 * All rights reserved
 *
 * Licensed under the Apache License, Version 2.0 (the ""License"");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an ""AS IS"" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "mgos_mqtt.h"

#include "mgos.h"
#include "mgos_mqtt_conn.h"
#include "mgos_sys_config.h"

#ifndef MGOS_MQTT_LOG_PUSHBACK_THRESHOLD
#define MGOS_MQTT_LOG_PUSHBACK_THRESHOLD 2048
#endif

#ifndef MGOS_MQTT_SUBSCRIBE_QOS
#define MGOS_MQTT_SUBSCRIBE_QOS 1
#endif

static struct mgos_mqtt_conn *s_conn = NULL;

extern uint16_t mgos_mqtt_conn_get_packet_id(struct mgos_mqtt_conn *c);
uint16_t mgos_mqtt_get_packet_id(void) {
  return mgos_mqtt_conn_get_packet_id(s_conn);
}

extern void mgos_mqtt_conn_sub_s(struct mgos_mqtt_conn *c, struct mg_str topic,
                                 int qos, mg_event_handler_t handler,
                                 void *user_data);
void mgos_mqtt_global_subscribe(const struct mg_str topic,
                                mg_event_handler_t handler, void *ud) {
  mgos_mqtt_conn_sub_s(s_conn, topic, MGOS_MQTT_SUBSCRIBE_QOS, handler, ud);
}

void mgos_mqtt_add_global_handler(mg_event_handler_t handler, void *ud) {
  mgos_mqtt_conn_add_handler(s_conn, handler, ud);
}

void mgos_mqtt_set_connect_fn(mgos_mqtt_connect_fn_t fn, void *fn_arg) {
  mgos_mqtt_conn_set_connect_fn(s_conn, fn, fn_arg);
}

static void s_debug_write_cb(int ev, void *ev_data, void *userdata) {
  if (s_conn == NULL) return;
  struct mgos_debug_hook_arg *arg = (struct mgos_debug_hook_arg *) ev_data;
  const char *topic =
      (arg->fd == 1
           ? mgos_sys_config_get_debug_stdout_topic()
           : arg->fd == 2 ? mgos_sys_config_get_debug_stderr_topic() : NULL);
  if (topic != NULL &&
      mgos_mqtt_num_unsent_bytes() < MGOS_MQTT_LOG_PUSHBACK_THRESHOLD) {
    static uint32_t s_seq = 0;
    char *msg = arg->buf;
    int msg_len = mg_asprintf(
        &msg, MGOS_DEBUG_TMP_BUF_SIZE, "%s %u %.3lf %d|%.*s",
        (mgos_sys_config_get_device_id() ? mgos_sys_config_get_device_id()
                                         : "-"),
        (unsigned int) s_seq, mg_time(), arg->fd, (int) arg->len,
        (const char *) arg->data);
    if (arg->len > 0) {
      mgos_mqtt_conn_pub(s_conn, topic, mg_mk_str_n(msg, msg_len), 0 /* qos */,
                         false /* retain */);
      s_seq++;
    }
    if (msg != arg->buf) free(msg);
  }

  (void) ev;
  (void) userdata;
}

bool mgos_mqtt_set_config(const struct mgos_config_mqtt *cfg) {
  if (s_conn == NULL) {
    s_conn = mgos_mqtt_conn_create(0, cfg);
    return true;
  }
  return mgos_mqtt_conn_set_config(s_conn, cfg);
}

bool mgos_mqtt_global_connect(void) {
  return mgos_mqtt_conn_connect(s_conn);
}

void mgos_mqtt_global_disconnect(void) {
  mgos_mqtt_conn_disconnect(s_conn);
}

bool mgos_mqtt_global_is_connected(void) {
  return mgos_mqtt_conn_is_connected(s_conn);
}

extern struct mg_connection *mgos_mqtt_conn_nc(struct mgos_mqtt_conn *c);

struct mg_connection *mgos_mqtt_get_global_conn(void) {
  return mgos_mqtt_conn_nc(s_conn);
}

uint16_t mgos_mqtt_pub(const char *topic, const void *message, size_t len,
                       int qos, bool retain) {
  return mgos_mqtt_conn_pub(s_conn, topic, mg_mk_str_n(message, len), qos,
                            retain);
}

uint16_t mgos_mqtt_pubf(const char *topic, int qos, bool retain,
                        const char *json_fmt, ...) {
  uint16_t res;
  va_list ap;
  va_start(ap, json_fmt);
  res = mgos_mqtt_conn_pubv(s_conn, topic, qos, retain, json_fmt, ap);
  va_end(ap);
  return res;
}

uint16_t mgos_mqtt_pubv(const char *topic, int qos, bool retain,
                        const char *json_fmt, va_list ap) {
  return mgos_mqtt_conn_pubv(s_conn, topic, qos, retain, json_fmt, ap);
}

struct sub_data {
  sub_handler_t handler;
  void *user_data;
};

static void mqttsubtrampoline(struct mg_connection *c, int ev, void *ev_data,
                              void *user_data) {
  if (ev != MG_EV_MQTT_PUBLISH) return;
  struct sub_data *sd = (struct sub_data *) user_data;
  struct mg_mqtt_message *mm = (struct mg_mqtt_message *) ev_data;
  sd->handler(c, mm->topic.p, mm->topic.len, mm->payload.p, mm->payload.len,
              sd->user_data);
}

void mgos_mqtt_sub(const char *topic, sub_handler_t handler, void *user_data) {
  struct sub_data *sd = (struct sub_data *) malloc(sizeof(*sd));
  sd->handler = handler;
  sd->user_data = user_data;
  mgos_mqtt_global_subscribe(mg_mk_str(topic), mqttsubtrampoline, sd);
}

bool mgos_mqtt_unsub(const char *topic) {
  return mgos_mqtt_conn_unsub(s_conn, topic);
}

size_t mgos_mqtt_num_unsent_bytes(void) {
  return mgos_mqtt_conn_num_unsent_bytes(s_conn);
}

bool mgos_mqtt_init(void) {
  if (mgos_sys_config_get_debug_stdout_topic() != NULL) {
    char *stdout_topic = strdup(mgos_sys_config_get_debug_stdout_topic());
    mgos_expand_mac_address_placeholders(stdout_topic);
    mgos_sys_config_set_debug_stdout_topic(stdout_topic);
    free(stdout_topic);
  }
  if (mgos_sys_config_get_debug_stderr_topic() != NULL) {
    char *stderr_topic = strdup(mgos_sys_config_get_debug_stderr_topic());
    mgos_expand_mac_address_placeholders(stderr_topic);
    mgos_sys_config_set_debug_stderr_topic(stderr_topic);
    free(stderr_topic);
  }

  if (mgos_sys_config_get_mqtt_enable()) {
    s_conn = mgos_mqtt_conn_create2(0, mgos_sys_config_get_mqtt(),
                                    mgos_sys_config_get_mqtt1());
  }

  mgos_event_add_handler(MGOS_EVENT_LOG, s_debug_write_cb, NULL);

  return true;
}
