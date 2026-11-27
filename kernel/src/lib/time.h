#pragma once
// calendar math, utc only. shared with user programs
#include <stdint.h>

struct datetime {
    int year, month, day;    // month and day start at 1
    int hour, min, sec;
    int wday;                // 0 = sunday
    int yday;                // 0 based
};

int64_t datetime_to_epoch(const struct datetime *d);
void epoch_to_datetime(int64_t t, struct datetime *d);
