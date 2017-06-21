let MQTT = {
  _sub: ffi('void mgos_mqtt_sub(char *, void (*)(void *, void *, int, void *, int, userdata), userdata)'),
  _subf: function(conn, topic, len1, msg, len2, ud) {
    return ud.cb(conn, fstr(topic, len1), fstr(msg, len2), ud.ud);
  },

  // ## **`MQTT.sub(topic, handler)`**
  // Subscribe to a topic, and call given handler function when message arrives.
  // A handler receives 4 parameters: MQTT connection, topic name,
  // message, and userdata.
  // Return value: none.
  //
  // Example:
  // ```javascript
  // load('api_mqtt.js');
  // MQTT.sub('my/topic/#', function(conn, topic, msg) {
  //   print('Topic:', topic, 'message:', msg);
  // }, null);
  // ```
  sub: function(topic, cb, ud) {
    return this._sub(topic, this._subf, { cb: cb, ud: ud });
  },

  _pub: ffi('int mgos_mqtt_pub(char *, void *, int, int)'),

  // ## **`MQTT.pub(topic, message, qos)`**
  // Publish message to a topic. QoS defaults to 0.
  // Return value: 0 on failure (e.g. no connection to server), 1 on success.
  //
  // Example - send MQTT message on button press:
  // ```javascript
  // load('api_mqtt.js');
  // load('api_gpio.js');
  // let pin = 0, topic = 'my/topic';
  // GPIO.set_button_handler(pin, GPIO.PULL_UP, GPIO.INT_EDGE_NEG, 200, function() {
  //   let res = MQTT.pub('my/topic', JSON.stringify({ a: 1, b: 2 }), 1);
  //   print('Published:', res ? 'yes' : 'no');
  // }, null);
  // ```
  pub: function(t, m, qos) {
    qos = qos || 0;
    return this._pub(t, m, m.length, qos);
  },

  // ## **`MQTT.setEventHandler(handler, userdata)`**
  // Set MQTT connection event handler. Event handler is
  // `ev_handler(conn, ev, edata)`, where `conn` is an opaque connection handle,
  // `ev` is an event number, `edata` is an event-specific data.
  // `ev` values could be low-level network events, like `Net.EV_CLOSE`
  // or `Net.EV_POLL`, or MQTT specific events, like `MQTT.EV_CONNACK`.
  //
  // Example:
  // ```javascript
  // MQTT.setEventHandler(function(conn, ev, edata) {
  //   if (ev !== 0) print('MQTT event handler: got', ev);
  // }, null);
  // ```
  setEventHandler: ffi('void mgos_mqtt_add_global_handler(void (*)(void *, int, void *, userdata), userdata)'),
};
