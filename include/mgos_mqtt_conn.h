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

#pragma once

#include <stdarg.h>
#include <stdbool.h>

#include "mgos_mongoose.h"
#include "mgos_sys_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque struct */
struct mgos_mqtt_conn;

/* Create a connection. */
struct mgos_mqtt_conn *mgos_mqtt_conn_create(
    int conn_id, const struct mgos_config_mqtt *cfg);

/* Create connection with failover config. */
struct mgos_mqtt_conn *mgos_mqtt_conn_create2(
    int conn_id, const struct mgos_config_mqtt *cfg,
    const struct mgos_config_mqtt *cfg2);

/* Use this to obtain mgos_mqtt_conn from mg_connection. */
struct mgos_mqtt_conn *mgos_mqtt_conn_from_nc(struct mg_connection *nc);

/* Registers a handler to be invoked on the MQTT connection. */
void mgos_mqtt_conn_add_handler(struct mgos_mqtt_conn *c,
                                mg_event_handler_t handler, void *user_data);

/*
 * Callback signature for `mgos_mqtt_set_connect_fn()`, see its docs for
 * details.
 */
typedef void (*mgos_mqtt_connect_fn_t)(struct mg_connection *nc,
                                       const char *client_id,
                                       struct mg_send_mqtt_handshake_opts *opts,
                                       void *fn_arg);

/*
 * Set connect callback. It is invoked when CONNECT message is about to
 * be sent. The callback is responsible to call `mg_send_mqtt_handshake_opt()`
 */
void mgos_mqtt_conn_set_connect_fn(struct mgos_mqtt_conn *c,
                                   mgos_mqtt_connect_fn_t fn, void *fn_arg);

/*
 * Attempt MQTT connection now (if enabled and not already connected).
 * Normally MQTT will try to connect in the background, at certain interval.
 * This function will force immediate connection attempt.
 */
bool mgos_mqtt_conn_connect(struct mgos_mqtt_conn *c);

/*
 * Disconnect from and/or stop trying to connect to MQTT server
 * until mgos_mqtt_conn_connect() is called.
 */
void mgos_mqtt_conn_disconnect(struct mgos_mqtt_conn *c);

/* Returns true if MQTT connection is up, false otherwise. */
bool mgos_mqtt_conn_is_connected(struct mgos_mqtt_conn *c);

/*
 * Publish message to the configured MQTT server, to the given MQTT topic.
 * Return value will be the packet id (> 0) if there is a connection to the
 * server and the message has been queued for sending. In case no connection is
 * available, 0 is returned. In case of QoS 1 return value does not indicate
 * that PUBACK has been received, use connection handler to check for that.
 */
uint16_t mgos_mqtt_conn_pub(struct mgos_mqtt_conn *c, const char *topic,
                            struct mg_str msg, int qos, bool retain);

/* Variants of mgos_mqtt_conn_pub for publishing a JSON-formatted string */
uint16_t mgos_mqtt_conn_pubv(struct mgos_mqtt_conn *c, const char *topic,
                             int qos, bool retain, const char *json_fmt,
                             va_list ap);
uint16_t mgos_mqtt_conn_pubf(struct mgos_mqtt_conn *c, const char *topic,
                             int qos, bool retain, const char *json_fmt, ...);

/*
 * Subscribe to a specific topic.
 * This handler will receive SUBACK - when first subscribed to the topic,
 * PUBLISH - for messages published to this topic.
 */
void mgos_mqtt_conn_sub(struct mgos_mqtt_conn *c, const char *topic, int qos,
                        mg_event_handler_t handler, void *user_data);

/*
 * Unsubscribe from a specific topic.
 */
bool mgos_mqtt_conn_unsub(struct mgos_mqtt_conn *c, const char *topic);

/*
 * Returns number of pending bytes to send.
 */
size_t mgos_mqtt_conn_num_unsent_bytes(struct mgos_mqtt_conn *c);

/*
 * Set maximum QOS level that is supported by server: 0, 1 or 2.
 * Some servers, particularly AWS GreenGrass, accept only QoS0 transactions.
 * An attempt to use any other QoS results into silent disconnect.
 * Therefore, instead of forcing all client code to track such server's quirks,
 * we add mechanism to transparently downgrade the QoS.
 */
void mgos_mqtt_conn_set_max_qos(struct mgos_mqtt_conn *c, int qos);

/*
 * (Re)configure MQTT.
 */
bool mgos_mqtt_conn_set_config(struct mgos_mqtt_conn *c,
                               const struct mgos_config_mqtt *cfg);

#ifdef __cplusplus
}
#endif
