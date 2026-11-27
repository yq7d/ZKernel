#include "tests/tests.h"
#include "kprintf.h"
#include "lib/printf.h"
#include "lib/string.h"
#include <stdint.h>

static int passed, failed;

#define CHECK(expected, ...) do {                                         \
        char buf[128];                                                    \
        int n = snprintf(buf, sizeof buf, __VA_ARGS__);                   \
        if (strcmp(buf, expected) == 0 && n == (int)strlen(expected)) {   \
            passed++;                                                     \
        } else {                                                          \
            failed++;                                                     \
            pr_err("printf test: %s -> \"%s\" (%d), expected \"%s\"\n",    \
                   #__VA_ARGS__, buf, n, expected);                       \
        }                                                                 \
    } while (0)

#pragma GCC diagnostic ignored "-Wformat"
#pragma GCC diagnostic ignored "-Wformat-extra-args"
#pragma GCC diagnostic ignored "-Wformat-overflow"

bool test_printf(void)
{
    passed = failed = 0;

    CHECK("hello", "hello");
    CHECK("42", "%d", 42);
    CHECK("-42", "%d", -42);
    CHECK("-2147483648", "%d", INT32_MIN);
    CHECK("-9223372036854775808", "%ld", INT64_MIN);
    CHECK("18446744073709551615", "%lu", UINT64_MAX);
    CHECK("ff", "%x", 255);
    CHECK("FF", "%X", 255);
    CHECK("0xff", "%#x", 255);
    CHECK("17", "%o", 15);
    CHECK("0b101", "%#b", 5);
    CHECK("   42", "%5d", 42);
    CHECK("42   |", "%-5d|", 42);
    CHECK("00042", "%05d", 42);
    CHECK("-0042", "%05d", -42);
    CHECK("+42", "%+d", 42);
    CHECK(" 42", "% d", 42);
    CHECK("  007", "%5.3d", 7);
    CHECK("", "%.0d", 0);
    CHECK("    7", "%*d", 5, 7);
    CHECK("7    |", "%*d|", -5, 7);
    CHECK("-1", "%hhd", 255);
    CHECK("65535", "%hu", 65535u);
    CHECK("x", "%c", 'x');
    CHECK("  x", "%3c", 'x');
    CHECK("abc", "%s", "abc");
    CHECK("  abc", "%5s", "abc");
    CHECK("abc  |", "%-5s|", "abc");
    CHECK("ab", "%.2s", "abc");
    CHECK("(null)", "%s", (char *)NULL);
    CHECK("100%", "%d%%", 100);
    CHECK("0x00000000deadbeef", "%p", (void *)0xdeadbeefUL);
    CHECK("123456789", "%zu", (size_t)123456789);
    CHECK("%q", "%q");

    // truncation
    char small[4];
    int n = snprintf(small, sizeof small, "%s", "abcdef");
    if (n == 6 && strcmp(small, "abc") == 0) {
        passed++;
    } else {
        failed++;
        pr_err("printf test: truncation -> \"%s\" (%d)\n", small, n);
    }

    if (failed)
        pr_err("printf: %d/%d tests passed\n", passed, passed + failed);
    else
        pr_ok("printf: all %d tests passed\n", passed);
    return failed == 0;
}
