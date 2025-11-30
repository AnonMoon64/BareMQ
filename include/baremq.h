#ifndef BAREMQ_H
#define BAREMQ_H
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct baremq_client baremq_client_t;

// Initialize client with optional username/password, keep-alive interval (seconds), and max buffer size
baremq_client_t *baremq_init(const char *broker_ip, uint16_t port, const char *client_id,
                             const char *username, const char *password, uint16_t keep_alive,
                             size_t max_buffer_size);

// Free client
void baremq_free(baremq_client_t *client);

// Connect to broker
int baremq_connect(baremq_client_t *client);

// Subscribe to a topic
int baremq_sub(baremq_client_t *client, const char *topic);

// Publish a message with QoS (0 or 1)
int baremq_send(baremq_client_t *client, const char *topic, const char *message, size_t message_len, int qos);

// Receive messages with callback
typedef void (*baremq_message_callback)(const char *topic, const char *message, size_t message_len);
int baremq_recv(baremq_client_t *client, baremq_message_callback callback);

// Reconnect on disconnection
int baremq_reconnect(baremq_client_t *client);

// Disconnect gracefully
int baremq_disconnect(baremq_client_t *client);

// Get last error message
const char *baremq_get_error(baremq_client_t *client);

// Server API: start IOCP-based server (port, max connections, per-connection buffer size)
int baremq_server_start_iocp(uint16_t port, size_t max_connections, size_t per_conn_buf);
void baremq_server_stop_iocp(void);

#ifdef __cplusplus
}
#endif

#endif