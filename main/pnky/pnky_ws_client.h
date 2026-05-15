#ifndef PNKY_WS_CLIENT_H
#define PNKY_WS_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_transport.h"

#define PNKY_WS_BUF_SIZE 4096
#define PNKY_WS_MAX_MSG_QUEUE 16

typedef struct {
    esp_transport_handle_t transport;
    char *host;
    int port;
    char *path;
    bool connected;
    bool use_tls;

    unsigned char pre_buf[2048];
    size_t pre_buf_len;

    char *recv_buffer;
    size_t recv_buf_len;
    size_t recv_buf_alloc;

    char *msg_queue[PNKY_WS_MAX_MSG_QUEUE];
    int msg_head;
    int msg_tail;
} pnky_ws_ctx_t;

pnky_ws_ctx_t *pnky_ws_connect(const char *host, int port, const char *path, bool use_tls);
bool pnky_ws_send(pnky_ws_ctx_t *ctx, const char *data, size_t len);
char *pnky_ws_recv_line(pnky_ws_ctx_t *ctx, int timeout_sec);
bool pnky_ws_is_connected(pnky_ws_ctx_t *ctx);
void pnky_ws_disconnect(pnky_ws_ctx_t *ctx);
void pnky_ws_free(pnky_ws_ctx_t *ctx);

#endif
