#include "cmdline.h"
#include "lib/string.h"

static char buf[512];

void cmdline_init(const char *cmdline)
{
    strlcpy(buf, cmdline ? cmdline : "", sizeof buf);
}

const char *cmdline_raw(void)
{
    return buf;
}

bool cmdline_get(const char *key, char *out, size_t size)
{
    size_t klen = strlen(key);
    const char *p = buf;

    while (*p) {
        while (*p == ' ')
            p++;
        const char *word = p;
        while (*p && *p != ' ')
            p++;
        size_t wlen = (size_t)(p - word);

        if (wlen >= klen && strncmp(word, key, klen) == 0 &&
            (wlen == klen || word[klen] == '=')) {
            if (out && size) {
                const char *val = wlen == klen ? word + wlen : word + klen + 1;
                size_t vlen = (size_t)(word + wlen - val);
                if (vlen >= size)
                    vlen = size - 1;
                memcpy(out, val, vlen);
                out[vlen] = '\0';
            }
            return true;
        }
    }
    return false;
}
