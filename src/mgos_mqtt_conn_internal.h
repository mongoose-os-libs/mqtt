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

#pragma once

#include "common/queue.h"
#include "mgos_mqtt_conn.h"
#include "mgos_timers.h"

struct mgos_mqtt_subscription {
  struct mg_str topic;
  uint16_t sub_id;
  uint8_t qos;
  mg_event_handler_t handler;
  void *user_data;
  SLIST_ENTRY(mgos_mqtt_subscription) next;
};

struct mgos_mqtt_conn_handler {
  mg_event_handler_t handler;
  void *user_data;
  SLIST_ENTRY(mgos_mqtt_conn_handler) next;
};

struct mgos_mqtt_conn_queue_entry {
  char *topic;
  uint16_t packet_id;
  // DUP flag on queue entry is used to delay re-delivery.
  uint16_t flags;
  struct mg_str msg;
  STAILQ_ENTRY(mgos_mqtt_conn_queue_entry) next;
};

struct mgos_mqtt_ws_data;

struct mgos_mqtt_conn {
  const struct mgos_config_mqtt *cfg;
  const struct mgos_config_mqtt *cfg0;
  const struct mgos_config_mqtt *cfg1;
  struct mg_connection *nc;
  int reconnect_timeout_ms;
  mgos_timer_id reconnect_timer_id;
  int16_t conn_id;
  int16_t max_qos;
  uint16_t packet_id;
  bool connected;
  bool queue_drained;
  mgos_mqtt_connect_fn_t connect_fn;
  void *connect_fn_arg;
  struct mgos_mqtt_ws_data *ws_data;
  SLIST_HEAD(subscriptions, mgos_mqtt_subscription) subscriptions;
  SLIST_HEAD(conn_handlers, mgos_mqtt_conn_handler) conn_handlers;
  STAILQ_HEAD(queue, mgos_mqtt_conn_queue_entry) queue;
};

void mgos_mqtt_ev(struct mg_connection *nc, int ev, void *ev_data,
                  void *user_data);
void mgos_mqtt_ws_ev(struct mg_connection *nc, int ev, void *ev_data,
                     void *user_data);
void mgos_mqtt_conn_reconnect(struct mgos_mqtt_conn *c);
