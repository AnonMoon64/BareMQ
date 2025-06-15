#include "baremq.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void on_message(const char *topic, const char *message, size_t message_len) {
    printf("Received on '%s': %.*s\n", topic, (int)message_len, message);
}

int main() {
    // Max buffer size 10MB for large messages (Soulia chunks 100GB)
    baremq_client_t *client = baremq_init("broker.hivemq.com", 1883, "test-client", NULL, NULL, 60, 10 * 1024 * 1024);
    if (!client || baremq_connect(client) < 0) {
        printf("Error: %s\n", baremq_get_error(client));
        baremq_free(client);
        return 1;
    }
    printf("Connected to broker.hivemq.com:1883\n");
    if (baremq_sub(client, "soulia/+/commands") < 0) {
        printf("Error: %s\n", baremq_get_error(client));
        baremq_free(client);
        return 1;
    }
    printf("Subscribed to soulia/+/commands\n");

    // Test 1: Tiny 64-byte message
    size_t tiny_size = 64;
    char *tiny_message = malloc(tiny_size);
    if (!tiny_message) {
        printf("Error: Failed to allocate tiny message\n");
        baremq_free(client);
        return 1;
    }
    memset(tiny_message, 'C', tiny_size);
    printf("Sending %zu-byte message to soulia/bot1234/commands\n", tiny_size);
    if (baremq_send(client, "soulia/bot1234/commands", tiny_message, tiny_size, 1) < 0) {
        printf("Error: %s\n", baremq_get_error(client));
        free(tiny_message);
        baremq_free(client);
        return 1;
    }
    free(tiny_message);

    // Test 2: Small 256-byte message
    size_t small_size = 256;
    char *small_message = malloc(small_size);
    if (!small_message) {
        printf("Error: Failed to allocate small message\n");
        baremq_free(client);
        return 1;
    }
    memset(small_message, 'B', small_size);
    printf("Sending %zu-byte message to soulia/bot1234/commands\n", small_size);
    if (baremq_send(client, "soulia/bot1234/commands", small_message, small_size, 1) < 0) {
        printf("Error: %s\n", baremq_get_error(client));
        free(small_message);
        baremq_free(client);
        return 1;
    }
    free(small_message);

    baremq_recv(client, on_message);
    baremq_disconnect(client);
    baremq_free(client);
    return 0;
}