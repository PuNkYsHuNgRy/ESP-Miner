#include "pnky_ws_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_transport_tcp.h"
#include "esp_transport_ssl.h"
#include "mbedtls/base64.h"

static const char *TAG = "pnky_ws";

static void base64_encode(const unsigned char *in, size_t len, char *out, size_t out_size)
{
    size_t olen = 0;
    mbedtls_base64_encode((unsigned char *)out, out_size, &olen, in, len);
    out[olen] = '\0';
}

pnky_ws_ctx_t *pnky_ws_connect(const char *host, int port, const char *path, bool use_tls)
{
    pnky_ws_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;

    ctx->host = strdup(host);
    ctx->port = port;
    ctx->path = strdup(path ? path : "/");
    ctx->use_tls = use_tls;

    esp_transport_handle_t transport;
    if (use_tls) {
        transport = esp_transport_ssl_init();
        if (!transport) {
            ESP_LOGE(TAG, "Failed to init SSL transport");
            goto fail;
        }
        esp_transport_ssl_set_common_name(transport, host);
    } else {
        transport = esp_transport_tcp_init();
        if (!transport) {
            ESP_LOGE(TAG, "Failed to init TCP transport");
            goto fail;
        }
    }

    if (esp_transport_connect(transport, host, port, 5000) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to connect to %s:%d", host, port);
        esp_transport_close(transport);
        goto fail;
    }

    ctx->transport = transport;

    unsigned char nonce_bytes[16];
    for (int i = 0; i < 16; i++) nonce_bytes[i] = rand() & 0xFF;
    char nonce_b64[32];
    base64_encode(nonce_bytes, 16, nonce_b64, sizeof(nonce_b64));

    char req[1024];
    int len = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n",
        ctx->path, host, port, nonce_b64);

    if (esp_transport_write(transport, req, len, 5000) <= 0) {
        ESP_LOGE(TAG, "Failed to send WS upgrade request");
        goto fail;
    }

    char resp[4096];
    int total = 0;
    while (total < (int)sizeof(resp) - 1) {
        int n = esp_transport_read(transport, resp + total, sizeof(resp) - 1 - total, 5000);
        if (n <= 0) {
            ESP_LOGE(TAG, "Failed to read WS upgrade response");
            goto fail;
        }
        total += n;
        resp[total] = '\0';
        if (strstr(resp, "\r\n\r\n")) break;
    }

    if (total < 4 || !strstr(resp, " 101 ")) {
        ESP_LOGE(TAG, "WS upgrade rejected: %.200s", resp);
        goto fail;
    }

    char *body_start = strstr(resp, "\r\n\r\n");
    if (body_start) {
        body_start += 4;
        size_t body_len = total - (size_t)(body_start - resp);
        if (body_len > 0) {
            if (body_len > sizeof(ctx->pre_buf))
                body_len = sizeof(ctx->pre_buf);
            memcpy(ctx->pre_buf, body_start, body_len);
            ctx->pre_buf_len = body_len;
        }
    }

    ctx->connected = true;
    ESP_LOGI(TAG, "WS connected to %s:%d", host, port);
    return ctx;

fail:
    if (ctx->transport) { esp_transport_close(ctx->transport); }
    free(ctx->host);
    free(ctx->path);
    free(ctx);
    return NULL;
}

static bool ws_write_frame(pnky_ws_ctx_t *ctx, int opcode, const char *data, size_t len)
{
    unsigned char hdr[18];
    int hdrlen;
    unsigned char mask[4];
    for (int i = 0; i < 4; i++) mask[i] = rand() & 0xFF;

    hdr[0] = (unsigned char)(0x80 | opcode);

    if (len <= 125) {
        hdr[1] = (unsigned char)(0x80 | len);
        hdrlen = 2;
    } else if (len <= 65535) {
        hdr[1] = 0x80 | 126;
        hdr[2] = (unsigned char)(len >> 8);
        hdr[3] = (unsigned char)(len & 0xFF);
        hdrlen = 4;
    } else {
        hdr[1] = 0x80 | 127;
        hdr[2] = (unsigned char)((len >> 56) & 0xFF);
        hdr[3] = (unsigned char)((len >> 48) & 0xFF);
        hdr[4] = (unsigned char)((len >> 40) & 0xFF);
        hdr[5] = (unsigned char)((len >> 32) & 0xFF);
        hdr[6] = (unsigned char)((len >> 24) & 0xFF);
        hdr[7] = (unsigned char)((len >> 16) & 0xFF);
        hdr[8] = (unsigned char)((len >> 8) & 0xFF);
        hdr[9] = (unsigned char)(len & 0xFF);
        hdrlen = 10;
    }

    memcpy(hdr + hdrlen, mask, 4);
    hdrlen += 4;

    if (esp_transport_write(ctx->transport, (const char *)hdr, hdrlen, 5000) <= 0) {
        ESP_LOGE(TAG, "WS write hdr failed");
        ctx->connected = false;
        return false;
    }

    if (len > 0) {
        unsigned char *masked = malloc(len);
        if (!masked) return false;
        for (size_t i = 0; i < len; i++)
            masked[i] = ((const unsigned char *)data)[i] ^ mask[i % 4];
        int ret = esp_transport_write(ctx->transport, (const char *)masked, len, 5000);
        free(masked);
        if (ret <= 0) {
            ESP_LOGE(TAG, "WS write payload failed");
            ctx->connected = false;
            return false;
        }
    }

    return true;
}

static int ws_read_bytes(pnky_ws_ctx_t *ctx, unsigned char *buf, size_t len, int timeout_sec)
{
    size_t done = 0;
    int elapsed = 0;

    while (done < len) {
        if (ctx->pre_buf_len > 0) {
            size_t take = len - done;
            if (take > ctx->pre_buf_len) take = ctx->pre_buf_len;
            memcpy(buf + done, ctx->pre_buf, take);
            ctx->pre_buf_len -= take;
            if (ctx->pre_buf_len > 0)
                memmove(ctx->pre_buf, ctx->pre_buf + take, ctx->pre_buf_len);
            done += take;
            continue;
        }

        if (elapsed >= timeout_sec) return -2;
        int remaining = timeout_sec - elapsed;

        int n = esp_transport_read(ctx->transport, (char *)buf + done, (int)(len - done), remaining * 1000);
        if (n < 0) {
            if (errno == EAGAIN) { usleep(50000); elapsed++; continue; }
            return -1;
        }
        if (n == 0) { usleep(50000); elapsed++; continue; }
        done += n;
    }
    return 0;
}

bool pnky_ws_send(pnky_ws_ctx_t *ctx, const char *data, size_t len)
{
    if (!ctx || !ctx->connected || !ctx->transport) return false;
    return ws_write_frame(ctx, 0x01, data, len);
}

static void msg_enqueue(pnky_ws_ctx_t *ctx, const char *msg, size_t len)
{
    int next = (ctx->msg_head + 1) % PNKY_WS_MAX_MSG_QUEUE;
    if (next == ctx->msg_tail) return;
    ctx->msg_queue[ctx->msg_head] = malloc(len + 1);
    if (!ctx->msg_queue[ctx->msg_head]) return;
    memcpy(ctx->msg_queue[ctx->msg_head], msg, len);
    ctx->msg_queue[ctx->msg_head][len] = '\0';
    ctx->msg_head = next;
}

static char *msg_dequeue(pnky_ws_ctx_t *ctx)
{
    if (ctx->msg_tail == ctx->msg_head) return NULL;
    char *msg = ctx->msg_queue[ctx->msg_tail];
    ctx->msg_queue[ctx->msg_tail] = NULL;
    ctx->msg_tail = (ctx->msg_tail + 1) % PNKY_WS_MAX_MSG_QUEUE;
    return msg;
}

static int ws_read_one_frame(pnky_ws_ctx_t *ctx, char *buf, size_t bufsize,
                              int *out_opcode, int timeout_sec)
{
    unsigned char hdr[2];
    int rc = ws_read_bytes(ctx, hdr, 2, timeout_sec);
    if (rc == -2) return 0;
    if (rc < 0) return -1;

    *out_opcode = hdr[0] & 0x0F;
    bool masked = (hdr[1] & 0x80) != 0;
    unsigned long long payload_len = hdr[1] & 0x7F;

    if (payload_len == 126) {
        unsigned char ext[2];
        rc = ws_read_bytes(ctx, ext, 2, 1);
        if (rc < 0) return -1;
        payload_len = ((unsigned long long)ext[0] << 8) | ext[1];
    } else if (payload_len == 127) {
        unsigned char ext[8];
        rc = ws_read_bytes(ctx, ext, 8, 1);
        if (rc < 0) return -1;
        payload_len = 0;
        for (int i = 0; i < 8; i++)
            payload_len = (payload_len << 8) | ext[i];
    }

    unsigned char mask[4] = {0};
    if (masked) {
        rc = ws_read_bytes(ctx, mask, 4, 1);
        if (rc < 0) return -1;
    }

    if (payload_len > bufsize) {
        unsigned char tmp[256];
        for (unsigned long long i = 0; i < payload_len; i += sizeof(tmp)) {
            size_t chunk = payload_len - i;
            if (chunk > sizeof(tmp)) chunk = sizeof(tmp);
            ws_read_bytes(ctx, tmp, chunk, 1);
        }
        return 0;
    }

    if (payload_len == 0) return 0;

    rc = ws_read_bytes(ctx, (unsigned char *)buf, (size_t)payload_len, 1);
    if (rc < 0) return -1;

    if (masked) {
        for (size_t i = 0; i < payload_len; i++)
            buf[i] ^= mask[i % 4];
    }

    buf[payload_len] = '\0';
    return (int)payload_len;
}

static void parse_recv_buffer(pnky_ws_ctx_t *ctx)
{
    if (!ctx->recv_buffer || ctx->recv_buf_len == 0) return;

    char *line_start = ctx->recv_buffer;
    char *newline;
    while ((newline = memchr(line_start, '\n',
           ctx->recv_buf_len - (size_t)(line_start - ctx->recv_buffer))) != NULL) {
        size_t line_len = (size_t)(newline - line_start);
        if (line_len > 0 && line_start[line_len - 1] == '\r')
            line_len--;
        if (line_len > 0)
            msg_enqueue(ctx, line_start, line_len);
        line_start = newline + 1;
    }

    size_t consumed = (size_t)(line_start - ctx->recv_buffer);
    size_t remaining = ctx->recv_buf_len - consumed;
    if (remaining > 0 && consumed > 0)
        memmove(ctx->recv_buffer, line_start, remaining);
    ctx->recv_buf_len = remaining;
    if (ctx->recv_buffer)
        ctx->recv_buffer[ctx->recv_buf_len] = '\0';
}

char *pnky_ws_recv_line(pnky_ws_ctx_t *ctx, int timeout_sec)
{
    if (!ctx || !ctx->connected || !ctx->transport) return NULL;

    char *msg = msg_dequeue(ctx);
    if (msg) return msg;

    int elapsed = 0;
    while (elapsed < timeout_sec) {
        if (!ctx->connected) return NULL;

        int opcode = 0;
        char frame_buf[8192];
        int n = ws_read_one_frame(ctx, frame_buf, sizeof(frame_buf) - 1, &opcode, 1);

        if (n < 0) {
            ctx->connected = false;
            return NULL;
        }

        if (n == 0 && opcode == 0) {
            elapsed++;
            continue;
        }

        if (opcode == 0x08) {
            ESP_LOGI(TAG, "WS close frame received");
            ctx->connected = false;
            return NULL;
        }

        if (opcode == 0x09) {
            ws_write_frame(ctx, 0x0A, frame_buf, n);
            continue;
        }

        if (opcode == 0x01 || opcode == 0x02) {
            size_t need = ctx->recv_buf_len + (size_t)n + 2;
            if (need > ctx->recv_buf_alloc) {
                char *new_buf = realloc(ctx->recv_buffer, need);
                if (!new_buf) return NULL;
                ctx->recv_buffer = new_buf;
                ctx->recv_buf_alloc = need;
            }
            memcpy(ctx->recv_buffer + ctx->recv_buf_len, frame_buf, n);
            ctx->recv_buf_len += n;
            ctx->recv_buffer[ctx->recv_buf_len] = '\n';
            ctx->recv_buf_len++;
            ctx->recv_buffer[ctx->recv_buf_len] = '\0';

            parse_recv_buffer(ctx);
            msg = msg_dequeue(ctx);
            if (msg) return msg;
        }

        elapsed++;
    }

    return NULL;
}

bool pnky_ws_is_connected(pnky_ws_ctx_t *ctx)
{
    return ctx && ctx->connected && ctx->transport;
}

void pnky_ws_disconnect(pnky_ws_ctx_t *ctx)
{
    if (!ctx) return;

    if (ctx->connected && ctx->transport) {
        ws_write_frame(ctx, 0x08, "", 0);
    }

    ctx->connected = false;

    if (ctx->transport) {
        esp_transport_close(ctx->transport);
        ctx->transport = NULL;
    }

    free(ctx->recv_buffer);
    ctx->recv_buffer = NULL;
    ctx->recv_buf_len = 0;

    char *msg;
    while ((msg = msg_dequeue(ctx)) != NULL)
        free(msg);
    ctx->msg_head = 0;
    ctx->msg_tail = 0;
}

void pnky_ws_free(pnky_ws_ctx_t *ctx)
{
    if (!ctx) return;
    pnky_ws_disconnect(ctx);
    free(ctx->host);
    free(ctx->path);
    free(ctx);
}
