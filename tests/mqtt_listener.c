#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdint.h>
#include <conio.h> // For _kbhit, _getch

// Link with Winsock library
#pragma comment(lib, "Ws2_32.lib")

// MQTT packet types
#define MQTT_CONNECT 1
#define MQTT_CONNACK 2
#define MQTT_PUBLISH 3
#define MQTT_SUBSCRIBE 8
#define MQTT_SUBACK 9
#define MQTT_PINGREQ 12
#define MQTT_PINGRESP 13

// Structure for MQTT client
typedef struct {
    SOCKET sockfd;
    char *broker_ip;
    uint16_t broker_port;
    char *client_id;
} mqtt_client_t;

// Encode MQTT remaining length
int encode_remaining_length(uint8_t *buffer, size_t remaining_len) {
    int pos = 0;
    do {
        uint8_t digit = remaining_len % 128;
        remaining_len /= 128;
        if (remaining_len > 0) digit |= 0x80;
        buffer[pos++] = digit;
    } while (remaining_len > 0 && pos < 4);
    return pos;
}

// Initialize MQTT client
mqtt_client_t *mqtt_client_init(const char *broker_ip, uint16_t port, const char *client_id) {
    mqtt_client_t *client = malloc(sizeof(mqtt_client_t));
    if (!client) return NULL;
    
    client->broker_ip = strdup(broker_ip);
    client->broker_port = port;
    client->client_id = strdup(client_id);
    client->sockfd = INVALID_SOCKET;
    return client;
}

// Free MQTT client
void mqtt_client_free(mqtt_client_t *client) {
    if (client) {
        if (client->sockfd != INVALID_SOCKET) closesocket(client->sockfd);
        free(client->broker_ip);
        free(client->client_id);
        free(client);
    }
}

// Connect to TCP socket
int mqtt_tcp_connect(mqtt_client_t *client) {
    WSADATA wsaData;
    struct addrinfo hints, *result = NULL, *ptr = NULL;
    char port_str[6];
    int ret;

    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        fprintf(stderr, "WSAStartup failed: %d\n", WSAGetLastError());
        return -1;
    }

    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    snprintf(port_str, sizeof(port_str), "%u", client->broker_port);

    ret = getaddrinfo(client->broker_ip, port_str, &hints, &result);
    if (ret != 0) {
        fprintf(stderr, "getaddrinfo failed: %d (%s)\n", ret, gai_strerror(ret));
        WSACleanup();
        return -1;
    }

    client->sockfd = INVALID_SOCKET;
    for (ptr = result; ptr != NULL; ptr = ptr->ai_next) {
        client->sockfd = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
        if (client->sockfd == INVALID_SOCKET) {
            fprintf(stderr, "Socket creation failed: %d\n", WSAGetLastError());
            continue;
        }

        if (connect(client->sockfd, ptr->ai_addr, (int)ptr->ai_addrlen) == SOCKET_ERROR) {
            fprintf(stderr, "Connection failed: %d\n", WSAGetLastError());
            closesocket(client->sockfd);
            client->sockfd = INVALID_SOCKET;
            continue;
        }
        break;
    }

    freeaddrinfo(result);

    if (client->sockfd == INVALID_SOCKET) {
        fprintf(stderr, "Failed to connect to %s:%u\n", client->broker_ip, client->broker_port);
        WSACleanup();
        return -1;
    }

    return 0;
}

// Build MQTT CONNECT packet
int mqtt_build_connect_packet(uint8_t *buffer, size_t buffer_size, const char *client_id) {
    size_t client_id_len = strlen(client_id);
    size_t variable_header_len = 10;
    size_t payload_len = 2 + client_id_len;
    size_t remaining_len = variable_header_len + payload_len;
    
    uint8_t rem_len_buf[4];
    int rem_len_size = encode_remaining_length(rem_len_buf, remaining_len);
    if (buffer_size < 1 + rem_len_size + remaining_len) return -1;

    buffer[0] = (MQTT_CONNECT << 4);
    memcpy(buffer + 1, rem_len_buf, rem_len_size);

    buffer[1 + rem_len_size] = 0; buffer[2 + rem_len_size] = 4;
    buffer[3 + rem_len_size] = 'M'; buffer[4 + rem_len_size] = 'Q';
    buffer[5 + rem_len_size] = 'T'; buffer[6 + rem_len_size] = 'T';
    buffer[7 + rem_len_size] = 4;
    buffer[8 + rem_len_size] = 0;
    buffer[9 + rem_len_size] = 0; buffer[10 + rem_len_size] = 60;

    buffer[11 + rem_len_size] = (client_id_len >> 8) & 0xFF;
    buffer[12 + rem_len_size] = client_id_len & 0xFF;
    memcpy(buffer + 13 + rem_len_size, client_id, client_id_len);

    return 1 + rem_len_size + remaining_len;
}

// Send MQTT CONNECT packet and read CONNACK
int mqtt_connect(mqtt_client_t *client) {
    uint8_t buffer[256];
    int packet_len = mqtt_build_connect_packet(buffer, sizeof(buffer), client->client_id);
    if (packet_len < 0) {
        fprintf(stderr, "Failed to build CONNECT packet\n");
        return -1;
    }

    if (send(client->sockfd, (char *)buffer, packet_len, 0) != packet_len) {
        fprintf(stderr, "Failed to send CONNECT packet: %d\n", WSAGetLastError());
        return -1;
    }

    uint8_t response[4];
    int bytes_received = recv(client->sockfd, (char *)response, 4, 0);
    if (bytes_received != 4) {
        fprintf(stderr, "Failed to read CONNACK: %d\n", WSAGetLastError());
        return -1;
    }

    if ((response[0] >> 4) != MQTT_CONNACK || response[3] != 0) {
        fprintf(stderr, "Connection refused: %d\n", response[3]);
        return -1;
    }

    return 0;
}

// Build MQTT SUBSCRIBE packet
int mqtt_build_subscribe_packet(uint8_t *buffer, size_t buffer_size, const char *topic, uint16_t packet_id) {
    size_t topic_len = strlen(topic);
    size_t variable_header_len = 2;
    size_t payload_len = 2 + topic_len + 1;
    size_t remaining_len = variable_header_len + payload_len;

    uint8_t rem_len_buf[4];
    int rem_len_size = encode_remaining_length(rem_len_buf, remaining_len);
    if (buffer_size < 1 + rem_len_size + remaining_len) return -1;

    buffer[0] = (MQTT_SUBSCRIBE << 4) | 2;
    memcpy(buffer + 1, rem_len_buf, rem_len_size);

    buffer[1 + rem_len_size] = (packet_id >> 8) & 0xFF;
    buffer[2 + rem_len_size] = packet_id & 0xFF;

    buffer[3 + rem_len_size] = (topic_len >> 8) & 0xFF;
    buffer[4 + rem_len_size] = topic_len & 0xFF;
    memcpy(buffer + 5 + rem_len_size, topic, topic_len);
    buffer[5 + rem_len_size + topic_len] = 0;

    return 1 + rem_len_size + remaining_len;
}

// Subscribe to a topic
int mqtt_subscribe(mqtt_client_t *client, const char *topic, uint16_t packet_id) {
    uint8_t buffer[256];
    int packet_len = mqtt_build_subscribe_packet(buffer, sizeof(buffer), topic, packet_id);
    if (packet_len < 0) {
        fprintf(stderr, "Failed to build SUBSCRIBE packet\n");
        return -1;
    }

    if (send(client->sockfd, (char *)buffer, packet_len, 0) != packet_len) {
        fprintf(stderr, "Failed to send SUBSCRIBE packet: %d\n", WSAGetLastError());
        return -1;
    }

    uint8_t response[5];
    int bytes_received = recv(client->sockfd, (char *)response, 5, 0);
    if (bytes_received != 5) {
        fprintf(stderr, "Failed to read SUBACK: %d\n", WSAGetLastError());
        return -1;
    }

    if ((response[0] >> 4) != MQTT_SUBACK || response[4] > 2) {
        fprintf(stderr, "SUBACK failed: return code %d\n", response[4]);
        return -1;
    }

    return 0;
}

// Send PINGREQ and check PINGRESP
int mqtt_ping(mqtt_client_t *client) {
    int timeout = 3000; // 3 seconds
    setsockopt(client->sockfd, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout));

    uint8_t buffer[256];
    uint8_t pingreq[2] = { (MQTT_PINGREQ << 4), 0 };
    if (send(client->sockfd, (char *)pingreq, 2, 0) != 2) {
        fprintf(stderr, "Failed to send PINGREQ: %d\n", WSAGetLastError());
        return -1;
    }

    while (1) {
        int bytes_received = recv(client->sockfd, (char *)buffer, 2, 0);
        if (bytes_received <= 0) {
            fprintf(stderr, "Connection closed or error: %d\n", WSAGetLastError());
            return -1;
        }

        uint8_t packet_type = buffer[0] >> 4;
        size_t remaining_len = buffer[1];

        if (packet_type == MQTT_PINGRESP && remaining_len == 0) break;

        fprintf(stderr, "Received packet type %d, remaining length %zu, discarding\n", packet_type, remaining_len);
        if (remaining_len > sizeof(buffer) - 2) {
            fprintf(stderr, "Packet too large\n");
            return -1;
        }
        bytes_received = recv(client->sockfd, (char *)(buffer + 2), remaining_len, 0);
        if (bytes_received != (int)remaining_len) {
            fprintf(stderr, "Failed to read packet: %d\n", WSAGetLastError());
            return -1;
        }
    }

    timeout = 0;
    setsockopt(client->sockfd, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout));

    return 0;
}

// Read and parse PUBLISH packet
void mqtt_listen(mqtt_client_t *client) {
    uint8_t buffer[1024];
    uint32_t ping_timer = GetTickCount();

    printf("Press 'q' to quit\n");
    while (1) {
        // Check for keypress
        if (_kbhit()) {
            if (_getch() == 'q') break;
        }

        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(client->sockfd, &read_fds);
        struct timeval timeout = { .tv_sec = 1, .tv_usec = 0 };

        int select_result = select(client->sockfd + 1, &read_fds, NULL, NULL, &timeout);
        if (select_result == SOCKET_ERROR) {
            fprintf(stderr, "Select failed: %d\n", WSAGetLastError());
            break;
        }

        // Send PINGREQ every 30 seconds
        if (GetTickCount() - ping_timer >= 30000) {
            if (mqtt_ping(client) < 0) break;
            ping_timer = GetTickCount();
        }

        if (select_result == 0) continue;

        int bytes_received = recv(client->sockfd, (char *)buffer, 2, 0);
        if (bytes_received <= 0) {
            fprintf(stderr, "Connection closed or error: %d\n", WSAGetLastError());
            break;
        }

        if ((buffer[0] >> 4) != MQTT_PUBLISH) continue;

        size_t remaining_len = buffer[1];
        if (remaining_len > sizeof(buffer) - 2) {
            fprintf(stderr, "Message too large\n");
            continue;
        }

        bytes_received = recv(client->sockfd, (char *)(buffer + 2), remaining_len, 0);
        if (bytes_received != (int)remaining_len) {
            fprintf(stderr, "Failed to read PUBLISH packet: %d\n", WSAGetLastError());
            continue;
        }

        uint16_t topic_len = (buffer[2] << 8) | buffer[3];
        char *topic = malloc(topic_len + 1);
        if (!topic) continue;
        memcpy(topic, buffer + 4, topic_len);
        topic[topic_len] = '\0';

        size_t message_len = remaining_len - 2 - topic_len;
        char *message = malloc(message_len + 1);
        if (!message) {
            free(topic);
            continue;
        }
        memcpy(message, buffer + 4 + topic_len, message_len);
        message[message_len] = '\0';

        printf("Received on '%s': %s\n", topic, message);

        free(topic);
        free(message);
    }
}

int main() {
    mqtt_client_t *client = mqtt_client_init("broker.hivemq.com", 1883, "listener-client");
    if (!client) {
        fprintf(stderr, "Failed to initialize client\n");
        return 1;
    }

    if (mqtt_tcp_connect(client) < 0) {
        mqtt_client_free(client);
        WSACleanup();
        return 1;
    }

    if (mqtt_connect(client) < 0) {
        mqtt_client_free(client);
        WSACleanup();
        return 1;
    }

    printf("Connected to MQTT broker\n");

    if (mqtt_subscribe(client, "commands", 1) < 0) {
        mqtt_client_free(client);
        WSACleanup();
        return 1;
    }
    printf("Subscribed to 'commands'\n");

    printf("Listening for messages...\n");
    mqtt_listen(client);

    mqtt_client_free(client);
    WSACleanup();
    return 0;
}