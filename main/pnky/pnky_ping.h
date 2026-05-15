#ifndef PNKY_PING_H
#define PNKY_PING_H

#include "global_state.h"

extern bool pnky_first_ping_done;
extern bool pnky_license_valid;

void pnky_ping_init(GlobalState *GLOBAL_STATE);
bool pnky_send_ping(GlobalState *GLOBAL_STATE);
void pnky_ping_task(void *pvParameters);

#endif
