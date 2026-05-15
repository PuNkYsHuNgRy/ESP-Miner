#ifndef PNKY_LICENSE_H
#define PNKY_LICENSE_H

#include <stdbool.h>
#include "pnky_ping.h"

static inline bool pnky_license_is_valid(void) {
    return pnky_license_valid;
}

#endif
