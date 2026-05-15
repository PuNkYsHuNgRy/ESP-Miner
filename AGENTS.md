# ESP-Miner PNKY Integration Notes

## Overview

This is a fork of [bitaxeorg/ESP-Miner](https://github.com/bitaxeorg/ESP-Miner) with PNKY (NoRugPull) modules added for the Bitaxe BM1370 ASIC miner.

## PNKY Changes

### New Files (main/pnky/)

- `pnky_ws_client.h/c` — WebSocket client on top of ESP-IDF esp_transport
  - Connects via WSS to btcpool.punkyshungry.com:443
  - Handles WebSocket upgrade, frame masking, text frame send/recv
  - Buffer management for fragmented frames

- `pnky_config.h/c` — PNKY-specific NVS config
  - Separate NVS namespace "pnky" for Solana wallet, API key, challenge nonce, device ID
  - Auto-generates random 16-char hex device ID on first boot

- `pnky_ping.h/c` — Periodic HTTP ping task
  - POSTs to norugcoin.punkyshungry.com/api/v1/ping every 60s
  - Sends hashrate, shares, diff, firmware version
  - Handles 200/401 response (saves api_key + challenge_nonce)
  - Handles 409 re-registration (regenerates device ID on flag)

### Modified Files

- `main/global_state.h` — Added `pnky_ws_ctx_t *ws_ctx` field
- `main/main.c` — PNKY config init + ping task creation
- `main/CMakeLists.txt` — Added pnky sources, includes, esp_http_client, mbedtls deps
- `main/tasks/stratum_task.c` — Replaced raw TCP/TLS transport with WSS via WebSocket client
- `main/tasks/asic_result_task.c` — Share submission via WS (or fallback to raw transport)

## Architecture

```
Bitaxe → WSS btcpool.punkyshungry.com:443 → Cloudflare → ws_proxy → ckpool:3333
       → HTTP norugcoin.punkyshungry.com (ping) → Cloudflare → server:8000
```

## Stratum Flow (WSS)

1. `pnky_ws_connect(host, 443, "/", true)` — TCP+TLS + WS upgrade
2. Send `mining.configure` (JSON-RPC as WS text frame + \n)
3. Send `mining.subscribe` 
4. Send `mining.authorize` (username = btcwallet.deviceid)
5. Loop: `pnky_ws_recv_line()` → parse → handle notify/diff/share results
6. Share submission: build JSON in `asic_result_task.c`, send via `pnky_ws_send()`

## Fallback Mode

If using raw TCP (non-443 port), falls back to original `esp_transport` + `STRATUM_V1_*` functions.
Heartbeat task only created in fallback mode.

## Build

```bash
cd /root/norugpull/bitaxe-firmware
. ~/esp/v5.5.1/esp-idf/export.sh
idf.py build
```

## Config

Pool URL defaults to WSS port 443 automatically. Configure via Axe-OS web UI or NVS:

- `NVS_CONFIG_STRATUM_URL` = `btcpool.punkyshungry.com`
- `NVS_CONFIG_STRATUM_PORT` = `443`
- `NVS_CONFIG_STRATUM_TLS` = `2` (BUNDLED_CRT)

PNKY settings stored in NVS namespace "pnky":
- `solana_wallet` — user's Solana public key for PNKY rewards
- `api_key`, `challenge_nonce` — auth handshake (set by server)
- `device_id` — random 16-char hex, used as stratum worker name suffix
- `server_url` — default http://norugcoin.punkyshungry.com
