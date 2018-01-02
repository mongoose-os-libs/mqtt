# Generic MQTT server

This library provides [MQTT protocol](https://en.wikipedia.org/wiki/MQTT)
API that allows devices to talk to MQTT servers.

Mongoose OS implements MQTT 3.1.1 client functionality, and works with
all popular MQTT server implementations, like AWS IoT, Google IoT Core,
Microsoft Azure, IBM Watson, HiveMQ, Mosquitto, etc.

In order to talk to an MQTT server, configure MQTT server settings -
see Configuration section below. Once configured, Mongoose OS keeps that
connection alive by reconnecting and re-subscribing to all topics
after disconnections - you do not need to implement the reconnection logic.

If you want to use TLS, set `mqtt.ssl_ca_cert=ca.pem`. Make sure that `ca.pem`
file has required CA certificates. If you want to use mutual TLS, set
`mqtt.ssl_cert=CLIENT_CERT.pem` and `mqtt.ssl_key=PRIVATE_KEY.pem`.

See example video (don't forget to set `mqtt.enable=true` before you try it):

<iframe src="https://www.youtube.com/embed/8dvpeonjmC0"
  width="560" height="315"  frameborder="0" allowfullscreen></iframe>

## Configuration

The MQTT library adds `mqtt` section to the device configuration:

```javascript
{
  "clean_session": true,        // Clean session info stored on server 
  "client_id": "",              // If not set, device.id is used
  "enable": false,              // Enable MQTT functionality
  "keep_alive": 60,             // How often to send PING messages in seconds
  "pass": "",                   // User password
  "reconnect_timeout_max": 60,  // Maximum reconnection timeout in seconds
  "reconnect_timeout_min": 2,   // Minimum reconnection timeout in seconds
  "server": "iot.eclipse.org:1883",    // SERVER:PORT to connect to
  "ssl_ca_cert": "",            // Set this to file name with CA certs to enable TLS
  "ssl_cert": "",               // Client certificate for mutual TLS
  "ssl_cipher_suites": "",      // TLS cipher suites
  "ssl_key": "",                // Private key for the client certificate
  "ssl_psk_identity": "",       // If set, a preshared key auth is used
  "ssl_psk_key": "",            // Preshared key
  "user": "",                   // MQTT user name, if MQTT auth is used
  "will_message": "",           // MQTT last will message
  "will_topic": ""              // MQTT last will topic
}
```
