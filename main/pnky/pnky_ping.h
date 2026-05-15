#ifndef PNKY_PING_H
#define PNKY_PING_H

#include "global_state.h"

#define PNKY_PING_INTERVAL_MS  60000
#define PNKY_API_KEY_LEN      65
#define PNKY_CHALLENGE_LEN    33
#define PNKY_WALLET_LEN       48

extern bool pnky_first_ping_done;
extern bool pnky_license_valid;

void pnky_ping_init(GlobalState *GLOBAL_STATE);
bool pnky_send_ping(GlobalState *GLOBAL_STATE);
void pnky_ping_task(void *pvParameters);
bool isPoolConnected(GlobalState *GLOBAL_STATE);

#endif
