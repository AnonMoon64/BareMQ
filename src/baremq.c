#include "baremq.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <conio.h>
#define CLOSE_SOCKET closesocket
#define SLEEP(ms) Sleep(ms)
#else
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/select.h>
#include <termios.h>
#include <fcntl.h>
#include <errno.h>
#define CLOSE_SOCKET close
#define SLEEP(ms) usleep((ms) * 1000)
#define SOCKET int
#define INVALID_SOCKET -1
#define SOCKET_ERROR -1
#define WSAGetLastError() errno
#endif

// MQTT packet types
#define MQTT_CONNECT 1
#define MQTT_CONNACK 2
#define MQTT_PUBLISH 3
#define MQTT_PUBACK 4
#define MQTT_SUBSCRIBE 8
#define MQTT_SUBACK 9
#define MQTT_PINGREQ 12
#define MQTT_PINGRESP 13
#define MQTT_DISCONNECT 14

#define MAX_PACKET_IDS 1024 // Pool size for packet IDs

// Internal client struct
struct baremq_client {
    SOCKET sockfd;
    char *broker_ip;
    uint16_t broker_port;
    char *client_id;
    char *username;
    char *password;
    uint16_t keep_alive;
    uint32_t last_ping;
    size_t max_buffer_size;
    char last_error[256];
    uint16_t packet_id_pool[MAX_PACKET_IDS];
    uint16_t next_packet_id_idx;
    uint16_t pending_pubacks[MAX_PACKET_IDS];
    size_t num_pending_pubacks;
};

// Decode MQTT remaining length (1-4 bytes)
static int decode_remaining_length(uint8_t *buffer, size_t *length, size_t *bytes_read, size_t available_bytes) {
    size_t multiplier = 1;
    size_t value = 0;
    size_t read = 0;
    uint8_t encoded_byte;

    do {
        if (read >= available_bytes || read >= 4) return -1; // Max 4 bytes or buffer limit
        encoded_byte = buffer[read++];
        value += (encoded_byte & 0x7F) * multiplier;
        multiplier *= 128;
    } while (encoded_byte & 0x80);

    *length = value;
    *bytes_read = read;
    return 0;
}

// Encode MQTT remaining length
static int encode_remaining_length(uint8_t *buffer, size_t remaining_len) {
    int pos = 0;
    do {
        uint8_t digit = remaining_len % 128;
        remaining_len /= 128;
        if (remaining_len > 0) digit |= 0x80;
        buffer[pos++] = digit;
    } while (remaining_len > 0 && pos < 4);
    return pos;
}

// Set last error
static void set_error(baremq_client_t *client, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(client->last_error, sizeof(client->last_error), fmt, args);
    va_end(args);
}

// Get next packet ID
static uint16_t get_packet_id(baremq_client_t *client) {
    if (client->num_pending_pubacks >= MAX_PACKET_IDS) {
        set_error(client, "Packet ID pool exhausted");
        return 0;
    }
    uint16_t pid = client->packet_id_pool[client->next_packet_id_idx];
    client->pending_pubacks[client->num_pending_pubacks++] = pid;
    client->next_packet_id_idx = (client->next_packet_id_idx + 1) % MAX_PACKET_IDS;
    return pid;
}

// Free packet ID
static void free_packet_id(baremq_client_t *client, uint16_t pid) {
    for (size_t i = 0; i < client->num_pending_pubacks; i++) {
        if (client->pending_pubacks[i] == pid) {
            client->pending_pubacks[i] = client->pending_pubacks[--client->num_pending_pubacks];
            break;
        }
    }
}

// Initialize client
baremq_client_t *baremq_init(const char *broker_ip, uint16_t port, const char *client_id,
                             const char *username, const char *password, uint16_t keep_alive,
                             size_t max_buffer_size) {
    baremq_client_t *client = malloc(sizeof(baremq_client_t));
    if (!client) return NULL;
    
    client->broker_ip = strdup(broker_ip);
    client->broker_port = port;
    client->client_id = strdup(client_id);
    client->username = username ? strdup(username) : NULL;
    client->password = password ? strdup(password) : NULL;
    client->keep_alive = keep_alive ? keep_alive : 60;
    client->max_buffer_size = max_buffer_size ? max_buffer_size : 1024 * 1024; // Default 1MB
    client->sockfd = INVALID_SOCKET;
    client->last_ping = 0;
    client->last_error[0] = '\0';
    client->next_packet_id_idx = 0;
    client->num_pending_pubacks = 0;
    for (uint16_t i = 0; i < MAX_PACKET_IDS; i++) {
        client->packet_id_pool[i] = i + 1; // IDs 1 to 1024
    }
    return client;
}

// Free client
void baremq_free(baremq_client_t *client) {
    if (client) {
        if (client->sockfd != INVALID_SOCKET) CLOSE_SOCKET(client->sockfd);
        free(client->broker_ip);
        free(client->client_id);
        free(client->username);
        free(client->password);
        free(client);
    }
}

// Connect to TCP socket
static int tcp_connect(baremq_client_t *client) {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        set_error(client, "WSAStartup failed: %d", WSAGetLastError());
        return -1;
    }
#endif

    struct addrinfo hints, *result = NULL, *ptr = NULL;
    char port_str[6];
    int ret;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    snprintf(port_str, sizeof(port_str), "%u", client->broker_port);

    ret = getaddrinfo(client->broker_ip, port_str, &hints, &result);
    if (ret != 0) {
        set_error(client, "getaddrinfo failed: %s", gai_strerror(ret));
#ifdef _WIN32
        WSACleanup();
#endif
        return -1;
    }

    client->sockfd = INVALID_SOCKET;
    for (ptr = result; ptr != NULL; ptr = ptr->ai_next) {
        client->sockfd = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
        if (client->sockfd == INVALID_SOCKET) continue;

        if (connect(client->sockfd, ptr->ai_addr, (int)ptr->ai_addrlen) == SOCKET_ERROR) {
            CLOSE_SOCKET(client->sockfd);
            client->sockfd = INVALID_SOCKET;
            continue;
        }
        break;
    }

    freeaddrinfo(result);

    if (client->sockfd == INVALID_SOCKET) {
        set_error(client, "Failed to connect to %s:%u", client->broker_ip, client->broker_port);
#ifdef _WIN32
        WSACleanup();
#endif
        return -1;
    }

    return 0;
}

// Build MQTT CONNECT packet
static int build_connect_packet(uint8_t *buffer, size_t buffer_size, baremq_client_t *client) {
    size_t client_id_len = strlen(client->client_id);
    size_t username_len = client->username ? strlen(client->client_id) : 0;
    size_t password_len = client->password ? strlen(client->password) : 0;
    size_t variable_header_len = 10;
    size_t payload_len = 2 + client_id_len;
    uint8_t connect_flags = 0;

    if (username_len) {
        payload_len += 2 + username_len;
        connect_flags |= 0x80;
    }
    if (password_len) {
        payload_len += 2 + password_len;
        connect_flags |= 0x40;
    }

    size_t remaining_len = variable_header_len + payload_len;
    
    uint8_t rem_len_buf[4];
    int rem_len_size = encode_remaining_length(rem_len_buf, remaining_len);
    if (buffer_size < 1 + rem_len_size + remaining_len) {
        set_error(client, "Buffer too small for CONNECT packet");
        return -1;
    }

    buffer[0] = (MQTT_CONNECT << 4);
    memcpy(buffer + 1, rem_len_buf, rem_len_size);

    size_t pos = 1 + rem_len_size;
    buffer[pos++] = 0; buffer[pos++] = 4;
    buffer[pos++] = 'M'; buffer[pos++] = 'Q';
    buffer[pos++] = 'T'; buffer[pos++] = 'T';
    buffer[pos++] = 4;
    buffer[pos++] = connect_flags;
    buffer[pos++] = (client->keep_alive >> 8) & 0xFF;
    buffer[pos++] = client->keep_alive & 0xFF;

    buffer[pos++] = (client_id_len >> 8) & 0xFF;
    buffer[pos++] = client_id_len & 0xFF;
    memcpy(buffer + pos, client->client_id, client_id_len);
    pos += client_id_len;

    if (username_len) {
        buffer[pos++] = (username_len >> 8) & 0xFF;
        buffer[pos++] = username_len & 0xFF;
        memcpy(buffer + pos, client->username, username_len);
        pos += username_len;
    }

    if (password_len) {
        buffer[pos++] = (password_len >> 8) & 0xFF;
        buffer[pos++] = password_len & 0xFF;
        memcpy(buffer + pos, client->password, password_len);
        pos += password_len;
    }

    return 1 + rem_len_size + remaining_len;
}

// Connect to broker
int baremq_connect(baremq_client_t *client) {
    if (tcp_connect(client) < 0) return -1;

    uint8_t *buffer = malloc(client->max_buffer_size);
    if (!buffer) {
        set_error(client, "Failed to allocate CONNECT buffer");
        return -1;
    }

    int packet_len = build_connect_packet(buffer, client->max_buffer_size, client);
    if (packet_len < 0) {
        free(buffer);
        return -1;
    }

    if (send(client->sockfd, (char *)buffer, packet_len, 0) != packet_len) {
        set_error(client, "Failed to send CONNECT packet: %d", WSAGetLastError());
        free(buffer);
        return -1;
    }

    uint8_t response[5];
    int bytes_received = recv(client->sockfd, (char *)response, 5, 0);
    fprintf(stderr, "CONNACK recv: %d bytes, error %d\n", bytes_received, WSAGetLastError());
    if (bytes_received != 4) {
        set_error(client, "Failed to read CONNACK: %d bytes, error %d", bytes_received, WSAGetLastError());
        free(buffer);
        return -1;
    }

    if ((response[0] >> 4) != MQTT_CONNACK) {
        set_error(client, "Invalid CONNACK packet: type %d", response[0] >> 4);
        free(buffer);
        return -1;
    }

    switch (response[3]) {
        case 0: break;
        case 1: set_error(client, "Connection refused: unacceptable protocol version"); free(buffer); return -1;
        case 2: set_error(client, "Connection refused: identifier rejected"); free(buffer); return -1;
        case 3: set_error(client, "Connection refused: server unavailable"); free(buffer); return -1;
        case 4: set_error(client, "Connection refused: bad username or password"); free(buffer); return -1;
        case 5: set_error(client, "Connection refused: not authorized"); free(buffer); return -1;
        default: set_error(client, "Connection refused: unknown error %d", response[3]); free(buffer); return -1;
    }

    free(buffer);
    client->last_ping = GetTickCount();
    return 0;
}

// Validate topic for wildcards
static int validate_topic(const char *topic) {
    size_t len = strlen(topic);
    for (size_t i = 0; i < len; i++) {
        if (topic[i] == '+' && (i > 0 && topic[i-1] != '/' || i < len-1 && topic[i+1] != '/')) {
            return -1;
        }
        if (topic[i] == '#' && i != len-1) {
            return -1;
        }
    }
    return 0;
}

// Build MQTT SUBSCRIBE packet
static int build_subscribe_packet(uint8_t *buffer, size_t buffer_size, const char *topic, uint16_t packet_id) {
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
int baremq_sub(baremq_client_t *client, const char *topic) {
    if (validate_topic(topic) < 0) {
        set_error(client, "Invalid topic wildcard syntax");
        return -1;
    }

    uint8_t *buffer = malloc(client->max_buffer_size);
    if (!buffer) {
        set_error(client, "Failed to allocate SUBSCRIBE buffer");
        return -1;
    }

    int packet_len = build_subscribe_packet(buffer, client->max_buffer_size, topic, client->packet_id_pool[client->next_packet_id_idx]);
    if (packet_len < 0) {
        set_error(client, "Failed to build SUBSCRIBE packet");
        free(buffer);
        return -1;
    }

    if (send(client->sockfd, (char *)buffer, packet_len, 0) != packet_len) {
        set_error(client, "Failed to send SUBSCRIBE packet: %d", WSAGetLastError());
        free(buffer);
        return -1;
    }

    uint8_t response[5];
    int bytes_received = recv(client->sockfd, (char *)response, 5, 0);
    fprintf(stderr, "SUBACK recv: %d bytes, error %d\n", bytes_received, WSAGetLastError());
    if (bytes_received != 5 || (response[0] >> 4) != MQTT_SUBACK || response[4] > 2) {
        set_error(client, "SUBACK failed: received %d bytes, type %d, return code %d", bytes_received, response[0] >> 4, response[4]);
        free(buffer);
        return -1;
    }

    free(buffer);
    client->last_ping = GetTickCount();
    return 0;
}

// Build MQTT PUBLISH packet
static int build_publish_packet(uint8_t *buffer, size_t buffer_size, const char *topic,
                                const char *message, size_t message_len, uint16_t packet_id, int qos) {
    size_t topic_len = strlen(topic);
    size_t variable_header_len = 2 + topic_len + (qos ? 2 : 0);
    size_t payload_len = message_len;
    size_t remaining_len = variable_header_len + payload_len;

    uint8_t rem_len_buf[4];
    int rem_len_size = encode_remaining_length(rem_len_buf, remaining_len);
    if (buffer_size < 1 + rem_len_size + remaining_len) return -1;

    buffer[0] = (MQTT_PUBLISH << 4) | (qos ? 2 : 0);
    memcpy(buffer + 1, rem_len_buf, rem_len_size);

    size_t pos = 1 + rem_len_size;
    buffer[pos++] = (topic_len >> 8) & 0xFF;
    buffer[pos++] = topic_len & 0xFF;
    memcpy(buffer + pos, topic, topic_len);
    pos += topic_len;

    if (qos) {
        buffer[pos++] = (packet_id >> 8) & 0xFF;
        buffer[pos++] = packet_id & 0xFF;
    }

    memcpy(buffer + pos, message, message_len);

    return 1 + rem_len_size + remaining_len;
}

// Read MQTT packet header (type + remaining length)
static int read_packet_header(SOCKET sockfd, uint8_t *type, size_t *remaining_len, uint8_t *header_buf, size_t *header_len) {
    int bytes_received = recv(sockfd, (char *)header_buf, 1, 0);
    if (bytes_received != 1) {
        return -1; // Error or connection closed
    }
    *type = header_buf[0] >> 4;
    *header_len = 1;

    // Read remaining length (1-4 bytes)
    uint8_t len_buf[4];
    size_t len_bytes = 0;
    uint8_t encoded_byte;
    do {
        if (len_bytes >= 4) return -1; // Invalid length
        bytes_received = recv(sockfd, (char *)&encoded_byte, 1, 0);
        if (bytes_received != 1) return -1;
        len_buf[len_bytes++] = encoded_byte;
        header_buf[*header_len] = encoded_byte;
        (*header_len)++;
    } while (encoded_byte & 0x80);

    size_t length, bytes_read;
    if (decode_remaining_length(len_buf, &length, &bytes_read, len_bytes) < 0) {
        return -1;
    }
    *remaining_len = length;
    return 0;
}

// Publish a message
int baremq_send(baremq_client_t *client, const char *topic, const char *message, size_t message_len, int qos) {
    if (qos != 0 && qos != 1) {
        set_error(client, "Invalid QoS level: %d", qos);
        return -1;
    }

    size_t total_size = 2 + strlen(topic) + (qos ? 2 : 0) + message_len + 4;
    if (total_size > client->max_buffer_size) {
        set_error(client, "Message size %zu exceeds max buffer size %zu", total_size, client->max_buffer_size);
        return -1;
    }

    uint8_t *buffer = malloc(client->max_buffer_size);
    if (!buffer) {
        set_error(client, "Failed to allocate PUBLISH buffer");
        return -1;
    }

    uint16_t pid = qos ? get_packet_id(client) : 0;
    if (qos && pid == 0) {
        free(buffer);
        return -1;
    }

    int packet_len = build_publish_packet(buffer, client->max_buffer_size, topic, message, message_len, pid, qos);
    if (packet_len < 0) {
        set_error(client, "Failed to build PUBLISH packet");
        if (qos) free_packet_id(client, pid);
        free(buffer);
        return -1;
    }

    if (send(client->sockfd, (char *)buffer, packet_len, 0) != packet_len) {
        set_error(client, "Failed to send PUBLISH packet: %d", WSAGetLastError());
        if (qos) free_packet_id(client, pid);
        free(buffer);
        return -1;
    }

    if (qos) {
        int timeout = 10000; // 10 seconds
        setsockopt(client->sockfd, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout));

        uint8_t header_buf[5];
        size_t header_len;
        int retries = 5;
        while (retries > 0) {
            uint8_t packet_type;
            size_t remaining_len;
            if (read_packet_header(client->sockfd, &packet_type, &remaining_len, header_buf, &header_len) < 0) {
                int err = WSAGetLastError();
                fprintf(stderr, "Header read failed: error %d\n", err);
#ifdef _WIN32
                if (err == WSAECONNRESET || err == WSAETIMEDOUT) {
                    set_error(client, "Connection issue (error %d), attempting reconnect", err);
                    baremq_reconnect(client);
                } else {
                    set_error(client, "Failed to read packet header: error %d", err);
                }
#else
                set_error(client, "Failed to read packet header: error %d", err);
#endif
                if (qos) free_packet_id(client, pid);
                free(buffer);
                return -1;
            }

            fprintf(stderr, "Received packet type %d, length %zu, header bytes %zu\n", packet_type, remaining_len, header_len);

            if (packet_type == MQTT_PUBACK && remaining_len == 2) {
                uint8_t puback_data[2];
                int bytes_received = recv(client->sockfd, (char *)puback_data, 2, 0);
                fprintf(stderr, "PUBACK data read: %d bytes, error %d\n", bytes_received, WSAGetLastError());
                if (bytes_received != 2) {
                    set_error(client, "Failed to read PUBACK data: %d bytes, error %d", bytes_received, WSAGetLastError());
                    if (qos) free_packet_id(client, pid);
                    free(buffer);
                    return -1;
                }
                uint16_t received_pid = (puback_data[0] << 8) | puback_data[1];
                if (received_pid == pid) break;
                set_error(client, "Received PUBACK with wrong packet ID: %d, expected %d", received_pid, pid);
                if (qos) free_packet_id(client, pid);
                free(buffer);
                return -1;
            }

            // Discard non-PUBACK packet
            fprintf(stderr, "Discarding packet type %d, length %zu\n", packet_type, remaining_len);
            if (remaining_len > client->max_buffer_size) {
                set_error(client, "Unexpected packet too large: %zu bytes", remaining_len);
                if (qos) free_packet_id(client, pid);
                free(buffer);
                return -1;
            }
            if (remaining_len > 0) {
                uint8_t *discard_buf = malloc(remaining_len);
                if (!discard_buf) {
                    set_error(client, "Failed to allocate discard buffer");
                    if (qos) free_packet_id(client, pid);
                    free(buffer);
                    return -1;
                }
                int bytes_received = recv(client->sockfd, (char *)discard_buf, remaining_len, 0);
                fprintf(stderr, "Discarded packet read: %d bytes, error %d\n", bytes_received, WSAGetLastError());
                if (bytes_received != (int)remaining_len) {
                    set_error(client, "Failed to read discarded packet: %d bytes, error %d", bytes_received, WSAGetLastError());
                    free(discard_buf);
                    if (qos) free_packet_id(client, pid);
                    free(buffer);
                    return -1;
                }
                free(discard_buf);
            }
            retries--;
            SLEEP(200);
        }

        if (retries == 0) {
            set_error(client, "Failed to receive valid PUBACK after retries");
            if (qos) free_packet_id(client, pid);
            free(buffer);
            return -1;
        }

        timeout = 0;
        setsockopt(client->sockfd, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout));
        free_packet_id(client, pid);
    }

    free(buffer);
    client->last_ping = GetTickCount();
    return 0;
}

// Send PINGREQ and check PINGRESP
static int ping(baremq_client_t *client) {
    int timeout = 3000;
    setsockopt(client->sockfd, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout));

    uint8_t pingreq[2] = { (MQTT_PINGREQ << 4), 0 };
    if (send(client->sockfd, (char *)pingreq, 2, 0) != 2) {
        set_error(client, "Failed to send PINGREQ: %d", WSAGetLastError());
        return -1;
    }

    uint8_t header_buf[5];
    size_t header_len;
    while (1) {
        uint8_t packet_type;
        size_t remaining_len;
        if (read_packet_header(client->sockfd, &packet_type, &remaining_len, header_buf, &header_len) < 0) {
            set_error(client, "Failed to read PINGRESP header: error %d", WSAGetLastError());
            return -1;
        }

        fprintf(stderr, "PINGRESP packet type %d, length %zu, header bytes %zu\n", packet_type, remaining_len, header_len);

        if (packet_type == MQTT_PINGRESP && remaining_len == 0) break;

        // Discard non-PINGRESP packet
        fprintf(stderr, "Discarding ping packet type %d, length %zu\n", packet_type, remaining_len);
        if (remaining_len > sizeof(header_buf)) {
            set_error(client, "Packet too large for ping buffer: %zu bytes", remaining_len);
            return -1;
        }
        if (remaining_len > 0) {
            int bytes_received = recv(client->sockfd, (char *)header_buf, remaining_len, 0);
            fprintf(stderr, "Discarded ping packet read: %d bytes, error %d\n", bytes_received, WSAGetLastError());
            if (bytes_received != (int)remaining_len) {
                set_error(client, "Failed to read discarded ping packet: %d", WSAGetLastError());
                return -1;
            }
        }
    }

    timeout = 0;
    setsockopt(client->sockfd, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout));

    client->last_ping = GetTickCount();
    return 0;
}

// Receive messages with callback
int baremq_recv(baremq_client_t *client, baremq_message_callback callback) {
    printf("Press 'q' to quit\n");
    while (1) {
#ifdef _WIN32
        if (_kbhit()) {
            if (_getch() == 'q') break;
        }
#else
        struct termios oldt, newt;
        tcgetattr(STDIN_FILENO, &oldt);
        newt = oldt;
        newt.c_lflag &= ~(ICANON | ECHO);
        tcsetattr(STDIN_FILENO, TCSANOW, &newt);
        fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);
        int ch = getchar();
        tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
        if (ch == 'q') break;
#endif

        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(client->sockfd, &read_fds);
        struct timeval timeout = { .tv_sec = 1, .tv_usec = 0 };

        int select_result = select(client->sockfd + 1, &read_fds, NULL, NULL, &timeout);
        if (select_result == SOCKET_ERROR) {
            set_error(client, "Select failed: %d", WSAGetLastError());
            baremq_reconnect(client);
            continue;
        }

        if (GetTickCount() - client->last_ping >= client->keep_alive * 1000) {
            if (ping(client) < 0) {
                baremq_reconnect(client);
                continue;
            }
        }

        if (select_result == 0) continue;

        uint8_t header_buf[5];
        size_t header_len;
        uint8_t packet_type;
        size_t remaining_len;
        if (read_packet_header(client->sockfd, &packet_type, &remaining_len, header_buf, &header_len) < 0) {
            set_error(client, "Failed to read PUBLISH header: error %d", WSAGetLastError());
            baremq_reconnect(client);
            continue;
        }

        fprintf(stderr, "PUBLISH packet type %d, length %zu, header bytes %zu\n", packet_type, remaining_len, header_len);

        if (packet_type == MQTT_PUBACK) {
            if (remaining_len != 2) {
                fprintf(stderr, "Invalid PUBACK length: %zu\n", remaining_len);
                continue;
            }
            uint8_t puback_data[2];
            int bytes_received = recv(client->sockfd, (char *)puback_data, 2, 0);
            fprintf(stderr, "PUBACK data read: %d bytes, error %d\n", bytes_received, WSAGetLastError());
            if (bytes_received != 2) continue;
            uint16_t pid = (puback_data[0] << 8) | puback_data[1];
            free_packet_id(client, pid);
            continue;
        }

        if (packet_type != MQTT_PUBLISH) {
            fprintf(stderr, "Skipping non-PUBLISH packet type %d, length %zu\n", packet_type, remaining_len);
            if (remaining_len > client->max_buffer_size) {
                set_error(client, "Non-PUBLISH packet too large: %zu bytes", remaining_len);
                continue;
            }
            if (remaining_len > 0) {
                uint8_t *discard_buf = malloc(remaining_len);
                if (!discard_buf) {
                    set_error(client, "Failed to allocate discard buffer");
                    continue;
                }
                int bytes_received = recv(client->sockfd, (char *)discard_buf, remaining_len, 0);
                fprintf(stderr, "Discarded packet read: %d bytes, error %d\n", bytes_received, WSAGetLastError());
                if (bytes_received != (int)remaining_len) {
                    free(discard_buf);
                    continue;
                }
                free(discard_buf);
            }
            continue;
        }

        uint8_t *buffer = malloc(client->max_buffer_size);
        if (!buffer) {
            set_error(client, "Failed to allocate RECEIVE buffer");
            continue;
        }

        if (remaining_len > client->max_buffer_size - 2) {
            set_error(client, "Message size %zu exceeds max buffer size %zu", remaining_len, client->max_buffer_size);
            size_t to_read = remaining_len;
            while (to_read > 0) {
                size_t chunk = to_read < sizeof(header_buf) ? to_read : sizeof(header_buf);
                int bytes_received = recv(client->sockfd, (char *)header_buf, chunk, 0);
                fprintf(stderr, "Draining large packet: %d bytes, error %d\n", bytes_received, WSAGetLastError());
                if (bytes_received <= 0) break;
                to_read -= bytes_received;
            }
            free(buffer);
            continue;
        }

        int bytes_received = recv(client->sockfd, (char *)buffer, remaining_len, 0);
        fprintf(stderr, "PUBLISH payload read: %d bytes, error %d\n", bytes_received, WSAGetLastError());
        if (bytes_received != (int)remaining_len) {
            set_error(client, "Failed to read PUBLISH packet: %d bytes, error %d", bytes_received, WSAGetLastError());
            free(buffer);
            continue;
        }

        uint16_t topic_len = (buffer[0] << 8) | buffer[1];
        char *topic = malloc(topic_len + 1);
        if (!topic) {
            free(buffer);
            continue;
        }
        memcpy(topic, buffer + 2, topic_len);
        topic[topic_len] = '\0';

        size_t message_len = remaining_len - 2 - topic_len;
        char *message = malloc(message_len + 1);
        if (!message) {
            free(topic);
            free(buffer);
            continue;
        }
        memcpy(message, buffer + 2 + topic_len, message_len);
        message[message_len] = '\0';

        callback(topic, message, message_len);

        free(topic);
        free(message);
        free(buffer);
        client->last_ping = GetTickCount();
    }
    return 0;
}

// Reconnect on disconnection
int baremq_reconnect(baremq_client_t *client) {
    CLOSE_SOCKET(client->sockfd);
    client->sockfd = INVALID_SOCKET;

    int retries = 0;
    int delay = 1000;
    while (retries < 5) {
        if (baremq_connect(client) == 0) {
            baremq_sub(client, "soulia/+/commands");
            return 0;
        }
        SLEEP(delay);
        delay *= 2;
        if (delay > 60000) delay = 60000;
        retries++;
    }

    set_error(client, "Reconnect failed after %d attempts", retries);
    return -1;
}

// Disconnect gracefully
int baremq_disconnect(baremq_client_t *client) {
    uint8_t buffer[2] = { (MQTT_DISCONNECT << 4), 0 };
    send(client->sockfd, (char *)buffer, 2, 0);
    CLOSE_SOCKET(client->sockfd);
    client->sockfd = INVALID_SOCKET;
    return 0;
}

// Get last error message
const char *baremq_get_error(baremq_client_t *client) {
    return client->last_error;
}