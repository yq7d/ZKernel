#pragma once
#include <stdbool.h>
#include <stdint.h>

void rtc_init(void);
// seconds since 1970, utc
int64_t rtc_now(void);
