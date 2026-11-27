// date [+%s]
#include "zk.h"
#include "lib/time.h"

int main(int argc, char **argv)
{
    long t = time();
    if (argc > 1 && strcmp(argv[1], "+%s") == 0) {
        printf("%ld\n", t);
        return 0;
    }
    struct datetime d;
    epoch_to_datetime(t, &d);
    static const char *days[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char *mon[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    printf("%s %s %2d %02d:%02d:%02d UTC %d\n", days[d.wday], mon[d.month - 1], d.day,
           d.hour, d.min, d.sec, d.year);
    return 0;
}
