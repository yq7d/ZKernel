#include "tests/tests.h"
#include "kprintf.h"
#include "lib/string.h"

static int passed, failed;

#define CHECK(cond) do {                                  \
        if (cond) passed++;                               \
        else { failed++; pr_err("string test failed: %s\n", #cond); } \
    } while (0)

bool test_string(void)
{
    passed = failed = 0;
    char buf[32];

    CHECK(strlen("") == 0);
    CHECK(strlen("hello") == 5);
    CHECK(strnlen("hello", 3) == 3);
    CHECK(strcmp("abc", "abc") == 0);
    CHECK(strcmp("abc", "abd") < 0);
    CHECK(strcmp("b", "a") > 0);
    CHECK(strncmp("abcX", "abcY", 3) == 0);
    const char *h = "hello";
    CHECK(strchr(h, 'l') == h + 2);
    CHECK(strchr(h, 'z') == NULL);
    CHECK(strchr(h, '\0') == h + 5);
    CHECK(strrchr(h, 'l') == h + 3);

    memset(buf, 'x', sizeof buf);
    CHECK(buf[0] == 'x' && buf[31] == 'x');

    memcpy(buf, "0123456789", 11);
    CHECK(strcmp(buf, "0123456789") == 0);

    memmove(buf + 2, buf, 5);
    CHECK(memcmp(buf, "0101234789", 10) == 0);

    memcpy(buf, "0123456789", 11);
    memmove(buf, buf + 2, 5);
    CHECK(memcmp(buf, "2345656789", 10) == 0);

    CHECK(strlcpy(buf, "hello world", 6) == 11 && strcmp(buf, "hello") == 0);
    CHECK(strlcat(buf, "!!", sizeof buf) == 7 && strcmp(buf, "hello!!") == 0);

    if (failed)
        pr_err("string: %d/%d tests passed\n", passed, passed + failed);
    else
        pr_ok("string: all %d tests passed\n", passed);
    return failed == 0;
}
