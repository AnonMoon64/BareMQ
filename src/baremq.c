#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include "baremq.h"
#ifdef _MSC_VER
#define strdup _strdup
#define snprintf _snprintf
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdarg.h>

#define MAX_EVENTS 10000
#define BUFFER_SIZE 4096

// MQTT packet types
#define MQTT_CONNECT 1
#define MQTT_CONNACK 2
#define MQTT_PUBLISH 3
#define MQTT_PUBACK 4
#define MQTT_SUBSCRIBE 8
#define MQTT_SUBACK 9

// --- Lightweight IOCP-based acceptor and buffer pool (scalable path) ---
#include <mswsock.h>

#define MQTT_PINGREQ 12
#define MQTT_PINGRESP 13

#define MQTT_DISCONNECT 14


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
    char last_error[256];
    HANDLE iocp;
    size_t max_buffer_size;
    // receive buffer for assembling MQTT packets

    uint8_t *recv_buf;
    size_t recv_capacity;
    size_t recv_len;
    // user callback for incoming publishes
    void (*message_cb)(const char *topic, const char *message, size_t message_len);
    // packet id management and pending ack handles (indexed by packet id)

    HANDLE *pending_acks; // array size 65536
    uint16_t next_packet_id;
    CRITICAL_SECTION ack_lock;
    CRITICAL_SECTION recv_lock;
};

/* Global listen socket used by the server acceptor. Some functions in
    this file reference `g_listen_socket` while others reference
    `server_listen_sock`. Define this symbol and initialize to
    INVALID_SOCKET. */
static SOCKET g_listen_socket = INVALID_SOCKET;

/* Forward declaration to avoid implicit declaration warnings when
    `baremq_event_loop` is referenced before its definition. */
void baremq_event_loop(baremq_client_t *client);


// Decode MQTT Remaining Length. `data` points at the first length byte (i.e., byte 1 of header).
// `available` is how many bytes are available to read from `data` (not including fixed header byte0).
// Returns: on success, writes *out_len and *out_len_bytes and returns 0. If more bytes are needed, returns 1.
static int decode_remaining_length(const uint8_t *data, size_t available, size_t *out_len, int *out_len_bytes) {
    size_t multiplier = 1;
    size_t value = 0;
    int i = 0;
    for (i = 0; i < 4; ++i) {
        if ((size_t)i >= available) return 1; // need more bytes
        uint8_t encoded = data[i];
        value += (encoded & 127) * multiplier;
        if ((encoded & 128) == 0) {
            *out_len = value;
            *out_len_bytes = i + 1;

            return 0;
        }
        multiplier *= 128;
    }
    return -1; // malformed (length too long)
}

// ----------------- Server IOCP acceptor (per-operation model) -----------------

// Per-connection context for accepted sockets
// Minimal buffer pool structure
typedef struct BufferPool {
    char *pool;
    size_t buf_size;
    size_t count;
    size_t *free_stack;
    size_t free_top;
    CRITICAL_SECTION lock;
} BufferPool;

// Accept context for posting AcceptEx
typedef struct AcceptCtx {
    OVERLAPPED overlapped;
    SOCKET listen_sock;
    SOCKET accept_sock;
    char addrbuf[(sizeof(struct sockaddr_in6) + 16) * 2];
} AcceptCtx;

// Buffer pool helpers
static BufferPool *buffer_pool_create(size_t buf_size, size_t count) {
    BufferPool *bp = (BufferPool *)malloc(sizeof(BufferPool));
    if (!bp) return NULL;
    bp->buf_size = buf_size;
    bp->count = count;
    bp->pool = (char *)HeapAlloc(GetProcessHeap(), 0, buf_size * count);
    if (!bp->pool) { free(bp); return NULL; }
    bp->free_stack = (size_t *)malloc(sizeof(size_t) * count);
    if (!bp->free_stack) { HeapFree(GetProcessHeap(), 0, bp->pool); free(bp); return NULL; }
    for (size_t i = 0; i < count; ++i) bp->free_stack[i] = count - 1 - i;
    bp->free_top = count;
    InitializeCriticalSection(&bp->lock);
    return bp;
}

static void buffer_pool_destroy(BufferPool *bp) {
    if (!bp) return;
    DeleteCriticalSection(&bp->lock);
    HeapFree(GetProcessHeap(), 0, bp->pool);
    free(bp->free_stack);
    free(bp);
}

static ssize_t buffer_pool_acquire(BufferPool *bp) {
    ssize_t idx = -1;
    EnterCriticalSection(&bp->lock);
    if (bp->free_top > 0) idx = (ssize_t)bp->free_stack[--bp->free_top];
    LeaveCriticalSection(&bp->lock);
    return idx;
}

static void buffer_pool_release(BufferPool *bp, ssize_t idx) {
    if (idx < 0) return;
    EnterCriticalSection(&bp->lock);
    bp->free_stack[bp->free_top++] = (size_t)idx;
    LeaveCriticalSection(&bp->lock);
}

// create listen socket helper
static int create_listen_socket(uint16_t port) {
    SOCKET s = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
    if (s == INVALID_SOCKET) return -1;
    BOOL opt = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons(port);
    if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) == SOCKET_ERROR) { closesocket(s); return -1; }
    if (listen(s, SOMAXCONN) == SOCKET_ERROR) { closesocket(s); return -1; }
    g_listen_socket = s;
    return 0;
}

static AcceptCtx *g_accept_ctxs = NULL;
static size_t g_accept_ctx_count = 0;

typedef struct server_conn {
    SOCKET sock;
    BufferPool *bp;
    size_t buf_index;
    CRITICAL_SECTION lock;
    // user state pointer can be added here
    // accumulation buffer for framing
    uint8_t *accum;
    size_t accum_len;
    size_t accum_cap;
} server_conn;

typedef struct server_perio {
    OVERLAPPED overlapped;
    WSABUF buf;
    size_t buf_index;
    int op; // 0=recv,1=send
} server_perio;

static HANDLE server_iocp = NULL;
static SOCKET server_listen_sock = INVALID_SOCKET;
static BufferPool *server_bp = NULL;
static LPFN_ACCEPTEX server_lpfnAcceptEx = NULL;
static HANDLE *server_workers = NULL;
static size_t server_worker_count = 0;

// Utility: post an AcceptEx using an AcceptCtx-like overlapped stored in caller buffer
static int server_post_accept(OVERLAPPED *ov, SOCKET *accept_sock, char *addrbuf, size_t addrbuflen) {
    *accept_sock = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
    if (*accept_sock == INVALID_SOCKET) return -1;
    DWORD bytes = 0;
    BOOL rc = server_lpfnAcceptEx(server_listen_sock, *accept_sock, addrbuf, 0,
                                  sizeof(struct sockaddr_in) + 16,
                                  sizeof(struct sockaddr_in) + 16,
                                  &bytes, ov);
    if (!rc) {
        int err = WSAGetLastError();
        if (err != ERROR_IO_PENDING) {
            closesocket(*accept_sock);
            *accept_sock = INVALID_SOCKET;
            return -1;
        }
    }
    return 0;
}

static DWORD WINAPI server_iocp_worker(LPVOID lpParam) {
    (void)lpParam;
    DWORD bytes;
    ULONG_PTR key;
    LPOVERLAPPED overlapped;

    while (1) {
        BOOL ok = GetQueuedCompletionStatus(server_iocp, &bytes, &key, &overlapped, INFINITE);
        if (!ok) {
            if (!overlapped) continue;
        }
        if (!overlapped) continue;

        // Distinguish AcceptEx completion: for AcceptEx we don't use a connection key, so key==0
        // We can tell an AcceptEx by inspecting that overlapped comes from our addr buffer usage
        // For simplicity assume overlapped is for accept when key == 0
        if (key == 0) {
            AcceptCtx *actx = (AcceptCtx *)overlapped;
            SOCKET asock = actx->accept_sock;
            if (asock == INVALID_SOCKET) {
                // nothing to do
            } else {
                // update accept context
                setsockopt(asock, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, (char *)&server_listen_sock, sizeof(server_listen_sock));

                // create connection context
                server_conn *conn = (server_conn *)malloc(sizeof(server_conn));
                if (conn) {
                    conn->sock = asock;
                    conn->bp = server_bp;
                    conn->buf_index = (size_t)-1;
                    InitializeCriticalSection(&conn->lock);

                    // associate accepted socket with IOCP, use conn pointer as completion key
                    CreateIoCompletionPort((HANDLE)asock, server_iocp, (ULONG_PTR)conn, 0);

                    // post initial WSARecv
                    server_perio *pio = (server_perio *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(server_perio));
                    if (pio) {
                        // acquire one buffer for accumulation and one for the recv operation
                        ssize_t accum_idx = buffer_pool_acquire(server_bp);
                        ssize_t recv_idx = -1;
                        if (accum_idx >= 0) recv_idx = buffer_pool_acquire(server_bp);
                        if (accum_idx >= 0 && recv_idx >= 0) {
                            // assign accumulation buffer to conn
                            conn->buf_index = (size_t)accum_idx;
                            conn->accum = (uint8_t *)(server_bp->pool + (accum_idx * server_bp->buf_size));
                            conn->accum_cap = server_bp->buf_size;
                            conn->accum_len = 0;

                            // assign recv buffer to pio
                            pio->buf_index = (size_t)recv_idx;
                            pio->buf.buf = server_bp->pool + (recv_idx * server_bp->buf_size);
                            pio->buf.len = (ULONG)server_bp->buf_size;
                            pio->op = 0;
                            ZeroMemory(&pio->overlapped, sizeof(OVERLAPPED));
                            DWORD flags2 = 0;
                            DWORD recvBytes = 0;
                            int rr = WSARecv(asock, &pio->buf, 1, &recvBytes, &flags2, &pio->overlapped, NULL);
                            if (rr == SOCKET_ERROR) {
                                int we = WSAGetLastError();
                                if (we != WSA_IO_PENDING) {
                                    // release buffers and cleanup
                                    buffer_pool_release(server_bp, recv_idx);
                                    buffer_pool_release(server_bp, accum_idx);
                                    HeapFree(GetProcessHeap(), 0, pio);
                                    closesocket(asock);
                                    DeleteCriticalSection(&conn->lock);
                                    free(conn);
                                }
                            }
                        } else {
                            // not enough buffers available, cleanup
                            if (recv_idx >= 0) buffer_pool_release(server_bp, recv_idx);
                            if (accum_idx >= 0) buffer_pool_release(server_bp, accum_idx);
                            HeapFree(GetProcessHeap(), 0, pio);
                            closesocket(asock);
                            DeleteCriticalSection(&conn->lock);
                            free(conn);
                        }
                    }
                } else {
                    closesocket(asock);
                }
            }

            // Re-post AcceptEx on this accept context
            // create new accept socket and issue AcceptEx again
            actx->accept_sock = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
            ZeroMemory(&actx->overlapped, sizeof(OVERLAPPED));
            DWORD out2 = 0;
            BOOL rc2 = server_lpfnAcceptEx(server_listen_sock, actx->accept_sock, actx->addrbuf, 0,
                                           sizeof(struct sockaddr_in) + 16, sizeof(struct sockaddr_in) + 16, &out2, &actx->overlapped);
            if (!rc2) {
                int err2 = WSAGetLastError();
                if (err2 != ERROR_IO_PENDING) {
                    if (actx->accept_sock != INVALID_SOCKET) closesocket(actx->accept_sock);
                    actx->accept_sock = INVALID_SOCKET;
                }
            }

            continue;
        }

        // Otherwise key is server_conn*
        server_conn *conn = (server_conn *) (void *) key;
        server_perio *pio = (server_perio *) overlapped;
        if (bytes == 0) {
            // connection closed
            if (pio) {
                if (pio->buf_index != (size_t)-1) buffer_pool_release(server_bp, pio->buf_index);
                HeapFree(GetProcessHeap(), 0, pio);
            }
            EnterCriticalSection(&conn->lock);
            closesocket(conn->sock);
            LeaveCriticalSection(&conn->lock);
            DeleteCriticalSection(&conn->lock);
            if (conn->buf_index != (size_t)-1) buffer_pool_release(server_bp, conn->buf_index);
            free(conn);
            continue;
        }

        if (pio->op == 0) {
            // Received data in pio->buf.buf (bytes bytes). Append to conn->accum and parse MQTT frames.
            EnterCriticalSection(&conn->lock);
            if (!conn->accum) {
                // accumulation buffer should have been assigned from pool at accept; if missing, close connection
                LeaveCriticalSection(&conn->lock);
                if (pio->buf_index != (size_t)-1) buffer_pool_release(server_bp, pio->buf_index);
                HeapFree(GetProcessHeap(), 0, pio);
                EnterCriticalSection(&conn->lock);
                closesocket(conn->sock);
                LeaveCriticalSection(&conn->lock);
                if (conn->buf_index != (size_t)-1) buffer_pool_release(server_bp, conn->buf_index);
                DeleteCriticalSection(&conn->lock);
                free(conn);
                continue;
            }
            if (conn->accum_len + bytes > conn->accum_cap) {
                // accumulation overflow—drop connection to avoid reallocations
                LeaveCriticalSection(&conn->lock);
                if (pio->buf_index != (size_t)-1) buffer_pool_release(server_bp, pio->buf_index);
                HeapFree(GetProcessHeap(), 0, pio);
                EnterCriticalSection(&conn->lock);
                closesocket(conn->sock);
                LeaveCriticalSection(&conn->lock);
                if (conn->buf_index != (size_t)-1) buffer_pool_release(server_bp, conn->buf_index);
                DeleteCriticalSection(&conn->lock);
                free(conn);
                continue;
            }
            memcpy(conn->accum + conn->accum_len, pio->buf.buf, bytes);
            conn->accum_len += bytes;

            // parse loop
            while (1) {
                if (conn->accum_len < 2) break;
                size_t remaining_len = 0;
                int rem_bytes = 0;
                int dr = decode_remaining_length(conn->accum + 1, conn->accum_len - 1, &remaining_len, &rem_bytes);
                if (dr == 1) break; // need more bytes
                if (dr == -1) { /* malformed */ closesocket(conn->sock); break; }
                size_t total = 1 + rem_bytes + remaining_len;
                if (conn->accum_len < total) break;

                uint8_t *pkt = conn->accum;
                uint8_t pkt_type = pkt[0] >> 4;
                size_t var_idx = 1 + rem_bytes;
                if (pkt_type == MQTT_PUBLISH) {
                    if (var_idx + 2 <= total) {
                        uint16_t topic_len = (uint16_t)(pkt[var_idx] << 8) | pkt[var_idx+1];
                        size_t idx = var_idx + 2;
                        if (idx + topic_len <= total) {
                            // extract topic
                            char *topic = (char *)malloc(topic_len + 1);
                            memcpy(topic, &pkt[idx], topic_len);
                            topic[topic_len] = '\0';
                            idx += topic_len;
                            int qos = (pkt[0] >> 1) & 0x03;
                            uint16_t pid = 0;
                            if (qos > 0) {
                                if (idx + 2 <= total) {
                                    pid = (uint16_t)(pkt[idx] << 8) | pkt[idx+1];
                                    idx += 2;
                                }
                            }
                            size_t payload_len = total - idx;
                            const char *payload = (const char *)&pkt[idx];
                            // TODO: call higher-level handler; for now ignore payload
                            // If QoS1, send PUBACK via overlapped WSASend
                            if (qos == 1 && pid != 0) {
                                server_perio *sp = (server_perio *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(server_perio));
                                if (sp) {
                                    sp->op = 1;
                                    sp->buf_index = (size_t)-1;
                                    sp->buf.len = 4;
                                    sp->buf.buf = (char *)HeapAlloc(GetProcessHeap(), 0, 4);
                                    if (sp->buf.buf) {
                                        sp->buf.buf[0] = (MQTT_PUBACK << 4);
                                        sp->buf.buf[1] = 2;
                                        sp->buf.buf[2] = (uint8_t)((pid >> 8) & 0xFF);
                                        sp->buf.buf[3] = (uint8_t)(pid & 0xFF);
                                        DWORD sent = 0;
                                        DWORD flags2 = 0;
                                        int w = WSASend(conn->sock, &sp->buf, 1, &sent, flags2, &sp->overlapped, NULL);
                                        if (w == SOCKET_ERROR) {
                                            int we = WSAGetLastError();
                                            if (we != WSA_IO_PENDING) {
                                                buffer_pool_release(server_bp, sp->buf_index);
                                                HeapFree(GetProcessHeap(), 0, sp->buf.buf);
                                                HeapFree(GetProcessHeap(), 0, sp);
                                            }
                                        }
                                    } else {
                                        HeapFree(GetProcessHeap(), 0, sp);
                                    }
                                }
                            }
                            free(topic);
                        }
                    }
                } else if (pkt_type == MQTT_PINGREQ) {
                    // reply PINGRESP
                    uint8_t resp[2] = { (MQTT_PINGRESP << 4), 0 };
                    send(conn->sock, (char *)resp, 2, 0);
                } else if (pkt_type == MQTT_SUBSCRIBE) {
                    // For simplicity, accept subscribes with QoS0 granted
                    // SUBSCRIBE has packet id in variable header
                    if (var_idx + 2 <= total) {
                        uint16_t spid = (uint16_t)(pkt[var_idx] << 8) | pkt[var_idx+1];
                        // Build SUBACK with return code 0
                        uint8_t suback[5];
                        suback[0] = (MQTT_SUBACK << 4);
                        suback[1] = 3; // remaining len
                        suback[2] = (uint8_t)((spid >> 8) & 0xFF);
                        suback[3] = (uint8_t)(spid & 0xFF);
                        suback[4] = 0; // return QoS 0
                        send(conn->sock, (char *)suback, 5, 0);
                    }
                }

                // consume
                size_t left = conn->accum_len - total;
                if (left > 0) memmove(conn->accum, conn->accum + total, left);
                conn->accum_len = left;
            }

            LeaveCriticalSection(&conn->lock);

            // re-post recv
            ZeroMemory(&pio->overlapped, sizeof(OVERLAPPED));
            DWORD flags2 = 0;
            DWORD recvBytes = 0;
            int r = WSARecv(conn->sock, &pio->buf, 1, &recvBytes, &flags2, &pio->overlapped, NULL);
            if (r == SOCKET_ERROR) {
                int err = WSAGetLastError();
                if (err != WSA_IO_PENDING) {
                    if (pio->buf_index != (size_t)-1) buffer_pool_release(server_bp, pio->buf_index);
                    HeapFree(GetProcessHeap(), 0, pio);
                    EnterCriticalSection(&conn->lock);
                    closesocket(conn->sock);
                    LeaveCriticalSection(&conn->lock);
                    DeleteCriticalSection(&conn->lock);
                    if (conn->buf_index != (size_t)-1) buffer_pool_release(server_bp, conn->buf_index);
                    free(conn);
                }
            }
        } else {
            // send completion
            if (pio->buf_index != (size_t)-1) buffer_pool_release(server_bp, pio->buf_index);
            if (pio->buf.buf) HeapFree(GetProcessHeap(), 0, pio->buf.buf);
            HeapFree(GetProcessHeap(), 0, pio);
        }
    }
    return 0;
}

// Start server: create IOCP, load AcceptEx, post initial AcceptEx contexts, spawn workers
int baremq_server_start_iocp(uint16_t port, size_t max_connections, size_t per_conn_buf) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) return -1;
    server_iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (!server_iocp) return -1;
    server_bp = buffer_pool_create(per_conn_buf, max_connections);
    if (!server_bp) return -1;
     if (create_listen_socket(port) != 0) return -1;
     /* Keep the older name in sync so other code paths that use
         `server_listen_sock` will see the actual listen socket. */
     server_listen_sock = g_listen_socket;
     // load AcceptEx
    DWORD bytes = 0;
    GUID guid = WSAID_ACCEPTEX;
    if (WSAIoctl(g_listen_socket, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof(guid), &server_lpfnAcceptEx, sizeof(server_lpfnAcceptEx), &bytes, NULL, NULL) == SOCKET_ERROR) return -1;

    // associate listen socket with IOCP (use key NULL/0 for accept completions)
    CreateIoCompletionPort((HANDLE)g_listen_socket, server_iocp, (ULONG_PTR)0, 0);

    // post some AcceptEx contexts
    size_t accept_post = max_connections < 512 ? max_connections : 512;
    g_accept_ctx_count = accept_post;
    g_accept_ctxs = (AcceptCtx *)malloc(sizeof(AcceptCtx) * accept_post);
    memset(g_accept_ctxs, 0, sizeof(AcceptCtx) * accept_post);
    for (size_t i = 0; i < accept_post; ++i) {
        OverlappedZero:
        ZeroMemory(&g_accept_ctxs[i].overlapped, sizeof(OVERLAPPED));
        g_accept_ctxs[i].listen_sock = g_listen_socket;
        g_accept_ctxs[i].accept_sock = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
        if (g_accept_ctxs[i].accept_sock == INVALID_SOCKET) continue;
        DWORD out = 0;
        BOOL rc = server_lpfnAcceptEx(g_listen_socket, g_accept_ctxs[i].accept_sock, g_accept_ctxs[i].addrbuf, 0,
                                      sizeof(struct sockaddr_in) + 16, sizeof(struct sockaddr_in) + 16, &out, &g_accept_ctxs[i].overlapped);
        if (!rc) {
            int err = WSAGetLastError();
            if (err != ERROR_IO_PENDING) {
                closesocket(g_accept_ctxs[i].accept_sock);
                g_accept_ctxs[i].accept_sock = INVALID_SOCKET;
                continue;
            }
        }
    }

    // spawn worker threads
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    server_worker_count = si.dwNumberOfProcessors * 2;
    server_workers = (HANDLE *)malloc(sizeof(HANDLE) * server_worker_count);
    for (size_t i = 0; i < server_worker_count; ++i) {
        server_workers[i] = CreateThread(NULL, 0, server_iocp_worker, NULL, 0, NULL);
    }

    return 0;
}

// Stop server
void baremq_server_stop_iocp() {
    // TODO: gracefully stop posting accepts, close sockets, signal workers
    if (server_workers) {
        for (size_t i = 0; i < server_worker_count; ++i) {
            PostQueuedCompletionStatus(server_iocp, 0, 0, NULL);
        }
        for (size_t i = 0; i < server_worker_count; ++i) if (server_workers[i]) WaitForSingleObject(server_workers[i], INFINITE);
        free(server_workers); server_workers = NULL; server_worker_count = 0;
    }
    if (g_accept_ctxs) {
        for (size_t i = 0; i < g_accept_ctx_count; ++i) if (g_accept_ctxs[i].accept_sock != INVALID_SOCKET) closesocket(g_accept_ctxs[i].accept_sock);
        free(g_accept_ctxs); g_accept_ctxs = NULL; g_accept_ctx_count = 0;
    }
    if (server_bp) { buffer_pool_destroy(server_bp); server_bp = NULL; }
    if (server_iocp) { CloseHandle(server_iocp); server_iocp = NULL; }
    if (server_listen_sock != INVALID_SOCKET) { closesocket(server_listen_sock); server_listen_sock = INVALID_SOCKET; }
    WSACleanup();
}


// Packet ID helpers
static uint16_t get_next_packet_id(baremq_client_t *client) {
    EnterCriticalSection(&client->ack_lock);
    uint16_t id = client->next_packet_id++;
    if (client->next_packet_id == 0) client->next_packet_id = 1; // skip 0
    LeaveCriticalSection(&client->ack_lock);
    return id;

}

static int wait_for_ack(baremq_client_t *client, uint16_t packet_id, uint32_t timeout_ms) {
    if (packet_id == 0) return -1;
    EnterCriticalSection(&client->ack_lock);
    HANDLE ev = client->pending_acks[packet_id];

    if (!ev) {
        // create event
        ev = CreateEvent(NULL, TRUE, FALSE, NULL);
        if (!ev) { LeaveCriticalSection(&client->ack_lock); return -1; }
        client->pending_acks[packet_id] = ev;
    }
    LeaveCriticalSection(&client->ack_lock);


    DWORD rc = WaitForSingleObject(ev, timeout_ms);
    if (rc == WAIT_OBJECT_0) return 0;
    return -1;
}

static void signal_ack(baremq_client_t *client, uint16_t packet_id) {
    if (packet_id == 0) return;
    EnterCriticalSection(&client->ack_lock);
    HANDLE ev = client->pending_acks[packet_id];
    if (ev) {
        SetEvent(ev);
        // Close and clear to avoid reuse; callers create fresh events when waiting
        CloseHandle(ev);
        client->pending_acks[packet_id] = NULL;
    }

    LeaveCriticalSection(&client->ack_lock);
}

// Encode MQTT Remaining Length into buffer. Returns number of bytes written.
static int encode_remaining_length(uint8_t *buf, size_t len) {
    int idx = 0;
    do {
        uint8_t encoded = len % 128;
        len /= 128;
        // if there are more digits to encode, set the top bit of this digit
        if (len > 0) encoded |= 0x80;
        buf[idx++] = encoded;
    } while (len > 0 && idx < 4);

    return idx;
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
    client->sockfd = INVALID_SOCKET;
    client->last_ping = 0;
    client->last_error[0] = '\0';
    client->max_buffer_size = max_buffer_size;
    client->iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    // allocate receive buffer
    client->recv_capacity = client->max_buffer_size ? client->max_buffer_size : BUFFER_SIZE;
    client->recv_buf = (uint8_t *)malloc(client->recv_capacity);
    client->recv_len = 0;
    client->message_cb = NULL;
    client->pending_acks = (HANDLE *)calloc(65536, sizeof(HANDLE));
    client->next_packet_id = 1;
    InitializeCriticalSection(&client->ack_lock);
    InitializeCriticalSection(&client->recv_lock);
    return client;
}

// Free client
void baremq_free(baremq_client_t *client) {
    if (client) {
        if (client->sockfd != INVALID_SOCKET) closesocket(client->sockfd);
        free(client->broker_ip);
        free(client->client_id);
        free(client->username);
        free(client->password);
        if (client->recv_buf) free(client->recv_buf);
        if (client->pending_acks) {
            for (size_t i = 0; i < 65536; ++i) {
                if (client->pending_acks[i]) CloseHandle(client->pending_acks[i]);
            }
            free(client->pending_acks);
        }
        DeleteCriticalSection(&client->ack_lock);
        DeleteCriticalSection(&client->recv_lock);
        CloseHandle(client->iocp);
        free(client);
    }
}

// Set last error
static void set_error(baremq_client_t *client, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(client->last_error, sizeof(client->last_error), fmt, args);
    va_end(args);
}

// Send MQTT CONNECT packet
static int send_mqtt_connect(baremq_client_t *client) {
    uint8_t connect_packet[BUFFER_SIZE];
    uint8_t body[BUFFER_SIZE];
    size_t body_len = 0;

    // Variable header into body buffer
    const char *protocol_name = "MQTT";
    // Protocol name (2-byte length MSB, LSB)
    body[body_len++] = (uint8_t)((strlen(protocol_name) >> 8) & 0xFF);
    body[body_len++] = (uint8_t)(strlen(protocol_name) & 0xFF);
    memcpy(&body[body_len], protocol_name, strlen(protocol_name));
    body_len += strlen(protocol_name);
    // Protocol level
    body[body_len++] = 4; // MQTT 3.1.1

    // Connect flags — set clean session by default, and set username/password flags if present
    uint8_t connect_flags = 0x02; // Clean session
    size_t username_len = client->username ? strlen(client->username) : 0;
    size_t password_len = client->password ? strlen(client->password) : 0;
    if (username_len) connect_flags |= 0x80; // Username Flag
    if (password_len) connect_flags |= 0x40; // Password Flag
    body[body_len++] = connect_flags;

    // Keep alive (2 bytes)
    body[body_len++] = (uint8_t)((client->keep_alive >> 8) & 0xFF);
    body[body_len++] = (uint8_t)(client->keep_alive & 0xFF);

    // Payload: Client Identifier (2-byte length + data)
    size_t client_id_len = client->client_id ? strlen(client->client_id) : 0;
    body[body_len++] = (uint8_t)((client_id_len >> 8) & 0xFF);
    body[body_len++] = (uint8_t)(client_id_len & 0xFF);
    if (client_id_len) {
        memcpy(&body[body_len], client->client_id, client_id_len);
        body_len += client_id_len;
    }

    // Will topic/payload would go here if used (not supported in this simple client)

    // Username and Password (if present)
    if (username_len) {
        body[body_len++] = (uint8_t)((username_len >> 8) & 0xFF);
        body[body_len++] = (uint8_t)(username_len & 0xFF);
        memcpy(&body[body_len], client->username, username_len);
        body_len += username_len;
    }

    if (password_len) {
        body[body_len++] = (uint8_t)((password_len >> 8) & 0xFF);
        body[body_len++] = (uint8_t)(password_len & 0xFF);
        memcpy(&body[body_len], client->password, password_len);
        body_len += password_len;
    }

    // Build fixed header + remaining length
    size_t packet_len = 0;
    connect_packet[packet_len++] = (uint8_t)(MQTT_CONNECT << 4);

    // Encode remaining length into a temporary buffer
    uint8_t rem_buf[4];
    int rem_len_bytes = encode_remaining_length(rem_buf, body_len);
    if (rem_len_bytes <= 0) {
        set_error(client, "Failed to encode remaining length");
        return -1;
    }

    // Ensure total fits
    if (1 + rem_len_bytes + body_len > BUFFER_SIZE) {
        set_error(client, "CONNECT packet too large");
        return -1;
    }

    // copy remaining length
    for (int i = 0; i < rem_len_bytes; ++i) connect_packet[packet_len++] = rem_buf[i];

    // copy body
    memcpy(&connect_packet[packet_len], body, body_len);
    packet_len += body_len;

    // Send packet using WSASend (overlapped)
    WSABUF wsaBuf;
    DWORD bytesSent = 0;
    DWORD flags = 0;
    OVERLAPPED overlapped = {0};

    wsaBuf.buf = (char *)connect_packet;
    wsaBuf.len = (ULONG)packet_len;

    int result = WSASend(client->sockfd, &wsaBuf, 1, &bytesSent, flags, &overlapped, NULL);
    if (result == SOCKET_ERROR) {
        int error = WSAGetLastError();
        if (error != WSA_IO_PENDING) {
            set_error(client, "Failed to send CONNECT packet: WSAGetLastError=%d", error);
            return -1;
        }
        // If IO pending, the send was queued successfully.
    }

    return 0;
}

// Connect to broker using non-blocking sockets
int baremq_connect(baremq_client_t *client) {
    WSADATA wsaData;
    struct addrinfo *result = NULL, *ptr = NULL, hints;
    char port_str[6];
    int ret;

    if (WSAStartup(MAKEWORD(2,2), &wsaData) != 0) {
        set_error(client, "WSAStartup failed");
        return -1;
    }

    snprintf(port_str, sizeof(port_str), "%u", client->broker_port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    ret = getaddrinfo(client->broker_ip, port_str, &hints, &result);
    if (ret != 0) {
        set_error(client, "getaddrinfo: %s", gai_strerror(ret));
        WSACleanup();
        return -1;
    }

    for (ptr = result; ptr != NULL; ptr = ptr->ai_next) {
        client->sockfd = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
        if (client->sockfd == INVALID_SOCKET) continue;

        // Set socket to non-blocking
        u_long mode = 1;
        ioctlsocket(client->sockfd, FIONBIO, &mode);

        if (connect(client->sockfd, ptr->ai_addr, (int)ptr->ai_addrlen) != SOCKET_ERROR || WSAGetLastError() == WSAEWOULDBLOCK) break;

        closesocket(client->sockfd);
    }

    freeaddrinfo(result);

    if (ptr == NULL) {
        set_error(client, "Could not connect");
        WSACleanup();
        return -1;
    }

    // Associate the socket with the IO completion port
    CreateIoCompletionPort((HANDLE)client->sockfd, client->iocp, (ULONG_PTR)client, 0);

    // Send MQTT CONNECT packet
    if (send_mqtt_connect(client) != 0) {
        return -1;
    }

    return 0;
}

// Publish a message
int baremq_send(baremq_client_t *client, const char *topic, const char *message, size_t message_len, int qos) {
    if (!client || client->sockfd == INVALID_SOCKET || !topic) return -1;

    uint8_t body[BUFFER_SIZE];
    size_t body_len = 0;

    // Topic
    size_t topic_len = strlen(topic);
    if (topic_len + message_len + 10 > sizeof(body)) return -1;
    body[body_len++] = (uint8_t)((topic_len >> 8) & 0xFF);
    body[body_len++] = (uint8_t)(topic_len & 0xFF);
    memcpy(&body[body_len], topic, topic_len);
    body_len += topic_len;

    uint16_t packet_id = 0;
    if (qos == 1) {
        packet_id = get_next_packet_id(client);
        body[body_len++] = (uint8_t)((packet_id >> 8) & 0xFF);
        body[body_len++] = (uint8_t)(packet_id & 0xFF);
    }

    // payload
    if (message && message_len) {
        memcpy(&body[body_len], message, message_len);
        body_len += message_len;
    }

    // Build fixed header and remaining length
    uint8_t packet[BUFFER_SIZE];
    size_t p = 0;
    uint8_t fixed = (uint8_t)(MQTT_PUBLISH << 4);
    if (qos == 1) fixed |= (1 << 1);
    packet[p++] = fixed;

    uint8_t rem[4];
    int rem_bytes = encode_remaining_length(rem, body_len);
    if (rem_bytes <= 0) return -1;
    for (int i = 0; i < rem_bytes; ++i) packet[p++] = rem[i];
    memcpy(&packet[p], body, body_len);
    p += body_len;

    // If QoS1, create pending event before sending to avoid races
    if (qos == 1) {
        EnterCriticalSection(&client->ack_lock);
        if (client->pending_acks[packet_id] == NULL) {
            client->pending_acks[packet_id] = CreateEvent(NULL, TRUE, FALSE, NULL);
        }
        LeaveCriticalSection(&client->ack_lock);
    }

    WSABUF wsaBuf;
    DWORD sent = 0;
    DWORD flags = 0;
    OVERLAPPED ov = {0};
    wsaBuf.buf = (char *)packet;
    wsaBuf.len = (ULONG)p;
    int r = WSASend(client->sockfd, &wsaBuf, 1, &sent, flags, &ov, NULL);
    if (r == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err != WSA_IO_PENDING) {
            set_error(client, "WSASend failed: %d", err);
            return -1;
        }
    }

    if (qos == 1) {
        // wait for PUBACK (timeout 5s)
        int wait_ok = wait_for_ack(client, packet_id, 5000);
        if (wait_ok != 0) {
            set_error(client, "PUBACK timeout for packet id %u", packet_id);
            return -1;
        }
    }

    return 0;
}

// Subscribe to a topic (blocking until SUBACK or timeout)
int baremq_sub(baremq_client_t *client, const char *topic) {
    if (!client || client->sockfd == INVALID_SOCKET || !topic) return -1;
    uint8_t body[BUFFER_SIZE];
    size_t body_len = 0;
    uint16_t packet_id = get_next_packet_id(client);
    // packet id
    body[body_len++] = (uint8_t)((packet_id >> 8) & 0xFF);
    body[body_len++] = (uint8_t)(packet_id & 0xFF);
    // topic filter
    size_t topic_len = strlen(topic);
    body[body_len++] = (uint8_t)((topic_len >> 8) & 0xFF);
    body[body_len++] = (uint8_t)(topic_len & 0xFF);
    memcpy(&body[body_len], topic, topic_len);
    body_len += topic_len;
    // requested QoS = 0
    body[body_len++] = 0;

    uint8_t packet[BUFFER_SIZE];
    size_t p = 0;
    packet[p++] = (uint8_t)((MQTT_SUBSCRIBE << 4) | 0x02); // SUBSCRIBE with reserved bits
    uint8_t rem[4];
    int rem_bytes = encode_remaining_length(rem, body_len);
    if (rem_bytes <= 0) return -1;
    for (int i = 0; i < rem_bytes; ++i) packet[p++] = rem[i];
    memcpy(&packet[p], body, body_len);
    p += body_len;

    // create pending ack event
    EnterCriticalSection(&client->ack_lock);
    if (client->pending_acks[packet_id] == NULL) client->pending_acks[packet_id] = CreateEvent(NULL, TRUE, FALSE, NULL);
    LeaveCriticalSection(&client->ack_lock);

    WSABUF wsaBuf;
    DWORD sent = 0;
    DWORD flags = 0;
    OVERLAPPED ov = {0};
    wsaBuf.buf = (char *)packet;
    wsaBuf.len = (ULONG)p;
    int r = WSASend(client->sockfd, &wsaBuf, 1, &sent, flags, &ov, NULL);
    if (r == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err != WSA_IO_PENDING) {
            set_error(client, "WSASend SUBSCRIBE failed: %d", err);
            return -1;
        }
    }

    // wait for SUBACK (timeout 5s)
    int wait_ok = wait_for_ack(client, packet_id, 5000);
    if (wait_ok != 0) {
        set_error(client, "SUBACK timeout for packet id %u", packet_id);
        return -1;
    }
    return 0;
}

// Receive loop: set callback and run event loop (blocks)
int baremq_recv(baremq_client_t *client, baremq_message_callback callback) {
    if (!client) return -1;
    client->message_cb = callback;
    baremq_event_loop(client);
    return 0;
}

// Reconnect: close socket and attempt to reconnect
int baremq_reconnect(baremq_client_t *client) {
    if (!client) return -1;
    if (client->sockfd != INVALID_SOCKET) {
        closesocket(client->sockfd);
        client->sockfd = INVALID_SOCKET;
    }
    WSACleanup();
    // brief sleep
    Sleep(100);
    return baremq_connect(client);
}

// Disconnect gracefully
int baremq_disconnect(baremq_client_t *client) {
    uint8_t buffer[2] = { (MQTT_DISCONNECT << 4), 0 };
    send(client->sockfd, buffer, 2, 0);
    closesocket(client->sockfd);
    client->sockfd = INVALID_SOCKET;
    WSACleanup();
    return 0;
}

// Get last error message
const char *baremq_get_error(baremq_client_t *client) {
    return client->last_error;
}

// Additional functions for handling MQTT packets, subscribing, publishing, etc., would be implemented here.

// Structure to hold OVERLAPPED data and buffer
struct IOData {
    OVERLAPPED overlapped;
    WSABUF buffer;
    char data[BUFFER_SIZE];
};

// Main event loop using IO Completion Ports
void baremq_event_loop(baremq_client_t *client) {
    DWORD bytesTransferred;
    ULONG_PTR completionKey;
    LPOVERLAPPED overlapped;
    struct IOData *ioData;
    DWORD flags = 0;

    // Allocate IOData structure
    ioData = (struct IOData *)malloc(sizeof(struct IOData));
    if (!ioData) {
        set_error(client, "Failed to allocate IOData");
        return;
    }
    memset(ioData, 0, sizeof(struct IOData));
    ioData->buffer.buf = ioData->data;
    ioData->buffer.len = BUFFER_SIZE;

    // Post initial WSARecv
    if (WSARecv(client->sockfd, &ioData->buffer, 1, NULL, &flags, &ioData->overlapped, NULL) == SOCKET_ERROR) {
        if (WSAGetLastError() != WSA_IO_PENDING) {
            set_error(client, "WSARecv failed");
            free(ioData);
            return;
        }
    }

    while (GetQueuedCompletionStatus(client->iocp, &bytesTransferred, &completionKey, &overlapped, INFINITE)) {
        ioData = (struct IOData *)overlapped;

        if (!ioData) continue;

        if (bytesTransferred == 0) {
            set_error(client, "Connection closed by peer");
            free(ioData);
            return;
        }

        // Append received bytes into client's recv buffer
        EnterCriticalSection(&client->recv_lock);
        if (client->recv_len + bytesTransferred > client->recv_capacity) {
            // try to grow buffer up to a reasonable max
            size_t newcap = client->recv_capacity * 2;
            if (newcap < client->recv_len + bytesTransferred) newcap = client->recv_len + bytesTransferred;
            if (client->max_buffer_size && newcap > client->max_buffer_size) {
                set_error(client, "Receive buffer overflow");
                LeaveCriticalSection(&client->recv_lock);
                free(ioData);
                return;
            }
            uint8_t *nb = (uint8_t *)realloc(client->recv_buf, newcap);
            if (!nb) {
                set_error(client, "Failed to expand recv buffer");
                LeaveCriticalSection(&client->recv_lock);
                free(ioData);
                return;
            }
            client->recv_buf = nb;
            client->recv_capacity = newcap;
        }
        memcpy(client->recv_buf + client->recv_len, ioData->data, bytesTransferred);
        client->recv_len += bytesTransferred;
        LeaveCriticalSection(&client->recv_lock);

        // Try to parse as many full MQTT packets as possible
        while (1) {
            EnterCriticalSection(&client->recv_lock);
            if (client->recv_len < 2) { LeaveCriticalSection(&client->recv_lock); break; }
            size_t remaining_len = 0;
            int rem_len_bytes = 0;
            int dr = decode_remaining_length(client->recv_buf + 1, client->recv_len - 1, &remaining_len, &rem_len_bytes);
            if (dr == 1) { LeaveCriticalSection(&client->recv_lock); break; } // need more bytes
            if (dr == -1) { set_error(client, "Malformed remaining length"); LeaveCriticalSection(&client->recv_lock); free(ioData); return; }

            size_t total_packet_len = 1 + rem_len_bytes + remaining_len;
            if (client->recv_len < total_packet_len) { LeaveCriticalSection(&client->recv_lock); break; }

            // We have a full packet at client->recv_buf[0..total_packet_len-1]
            uint8_t *pkt = client->recv_buf;
            uint8_t packet_type = pkt[0] >> 4;
            size_t var_header_idx = 1 + rem_len_bytes;

            if (packet_type == MQTT_PUBACK) {
                if (remaining_len >= 2) {
                    uint16_t pid = (uint16_t)(pkt[var_header_idx] << 8) | pkt[var_header_idx + 1];
                    signal_ack(client, pid);
                }
            } else if (packet_type == MQTT_SUBACK) {
                if (remaining_len >= 2) {
                    uint16_t pid = (uint16_t)(pkt[var_header_idx] << 8) | pkt[var_header_idx + 1];
                    signal_ack(client, pid);
                }
            } else if (packet_type == MQTT_PINGRESP) {
                client->last_ping = GetTickCount();
            } else if (packet_type == MQTT_PUBLISH) {
                // parse topic
                if (var_header_idx + 2 > total_packet_len) {
                    // malformed
                } else {
                    uint16_t topic_len = (uint16_t)(pkt[var_header_idx] << 8) | pkt[var_header_idx + 1];
                    size_t idx = var_header_idx + 2;
                    if (idx + topic_len > total_packet_len) {
                        // malformed
                    } else {
                        char *topic_buf = (char *)malloc(topic_len + 1);
                        memcpy(topic_buf, &pkt[idx], topic_len);
                        topic_buf[topic_len] = '\0';
                        idx += topic_len;
                        int qos = (pkt[0] >> 1) & 0x03;
                        uint16_t pid = 0;
                        if (qos > 0) {
                            if (idx + 2 <= total_packet_len) {
                                pid = (uint16_t)(pkt[idx] << 8) | pkt[idx + 1];
                                idx += 2;
                            }
                        }
                        size_t payload_len = total_packet_len - idx;
                        const char *payload_ptr = (const char *)&pkt[idx];
                        if (client->message_cb) {
                            client->message_cb(topic_buf, payload_ptr, payload_len);
                        }
                        free(topic_buf);
                        // If QoS1, send PUBACK
                        if (qos == 1 && pid != 0) {
                            uint8_t ack_pkt[4];
                            ack_pkt[0] = (MQTT_PUBACK << 4);
                            ack_pkt[1] = 2; // remaining length
                            ack_pkt[2] = (uint8_t)((pid >> 8) & 0xFF);
                            ack_pkt[3] = (uint8_t)(pid & 0xFF);
                            send(client->sockfd, (char *)ack_pkt, 4, 0);
                        }
                    }
                }
            }

            // remove processed packet from buffer
            size_t remaining = client->recv_len - total_packet_len;
            if (remaining > 0) memmove(client->recv_buf, client->recv_buf + total_packet_len, remaining);
            client->recv_len = remaining;
            LeaveCriticalSection(&client->recv_lock);
        }

        // Re-post WSARecv for the next data
        memset(&ioData->overlapped, 0, sizeof(OVERLAPPED));
        if (WSARecv(client->sockfd, &ioData->buffer, 1, NULL, &flags, &ioData->overlapped, NULL) == SOCKET_ERROR) {
            if (WSAGetLastError() != WSA_IO_PENDING) {
                set_error(client, "WSARecv failed");
                free(ioData);
                return;
            }
        }
    }

    free(ioData);
}
