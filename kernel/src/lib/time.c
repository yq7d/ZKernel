#include "lib/time.h"

// days since 1970-01-01, proleptic gregorian
static int64_t days_from_civil(int64_t y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

int64_t datetime_to_epoch(const struct datetime *d)
{
    return days_from_civil(d->year, d->month, d->day) * 86400 +
           d->hour * 3600 + d->min * 60 + d->sec;
}

void epoch_to_datetime(int64_t t, struct datetime *d)
{
    int64_t days = t / 86400, secs = t % 86400;
    if (secs < 0) {
        secs += 86400;
        days--;
    }
    d->hour = (int)(secs / 3600);
    d->min = (int)(secs / 60 % 60);
    d->sec = (int)(secs % 60);
    d->wday = (int)((days % 7 + 11) % 7);    // 1970-01-01 was a thursday

    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    d->day = (int)(doy - (153 * mp + 2) / 5 + 1);
    d->month = (int)(mp < 10 ? mp + 3 : mp - 9);
    d->year = (int)(yoe + era * 400 + (d->month <= 2));
    d->yday = (int)(days - days_from_civil(d->year, 1, 1));
}
