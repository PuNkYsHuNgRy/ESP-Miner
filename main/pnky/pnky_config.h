#ifndef PNKY_CONFIG_H
#define PNKY_CONFIG_H

#include <stdbool.h>

#define PNKY_NVS_NAMESPACE "norugpull"
#define PNKY_DEVICE_ID_LEN 24
#define PNKY_BTC_WALLET "bc1q44ne07yvnyxddhjhjy6zd2s0kdxmuh7myw2h46"

typedef enum {
    PNKY_KEY_SOLANA_WALLET,
    PNKY_KEY_API_KEY,
    PNKY_KEY_CHALLENGE_NONCE,
    PNKY_KEY_DEVICE_ID,
    PNKY_KEY_SERVER_URL,
    PNKY_KEY_PING_INTERVAL,
    PNKY_KEY_COUNT
} pnky_config_key_t;

void pnky_config_init(void);
char *pnky_config_get_string(pnky_config_key_t key);
void pnky_config_set_string(pnky_config_key_t key, const char *value);
int pnky_config_get_int(pnky_config_key_t key);
void pnky_config_set_int(pnky_config_key_t key, int value);
void pnky_config_generate_device_id(void);
const char *pnky_config_get_device_id(void);

#endif
