#include "zk.h"

// environ starts out on the stack, the first change moves it to the heap.
// mine[i] marks strings this file allocated
static char **owned;
static bool *mine;
static size_t count, cap;

static size_t name_len(const char *s)
{
    size_t n = 0;
    while (s[n] && s[n] != '=')
        n++;
    return n;
}

static int find(const char *name, size_t n)
{
    for (int i = 0; environ && environ[i]; i++)
        if (name_len(environ[i]) == n && strncmp(environ[i], name, n) == 0)
            return i;
    return -1;
}

char *getenv(const char *name)
{
    int i = find(name, strlen(name));
    return i < 0 ? NULL : environ[i] + strlen(name) + 1;
}

static bool take_over(void)
{
    if (owned && environ == owned)
        return true;
    size_t n = 0;
    while (environ && environ[n])
        n++;
    size_t c = n + 8;
    char **v = malloc(c * sizeof(char *));
    bool *m = calloc(c, sizeof(bool));
    if (!v || !m) {
        free(v);
        free(m);
        return false;
    }
    for (size_t i = 0; i < n; i++)
        v[i] = environ[i];
    v[n] = NULL;
    free(owned);
    free(mine);
    owned = environ = v;
    mine = m;
    count = n;
    cap = c;
    return true;
}

int setenv(const char *name, const char *value, int overwrite)
{
    size_t n = strlen(name);
    if (!n || strchr(name, '='))
        return ZK_EINVAL;
    int i = find(name, n);
    if (i >= 0 && !overwrite)
        return 0;
    if (!take_over())
        return ZK_ENOMEM;

    size_t vl = strlen(value);
    char *s = malloc(n + vl + 2);
    if (!s)
        return ZK_ENOMEM;
    memcpy(s, name, n);
    s[n] = '=';
    memcpy(s + n + 1, value, vl + 1);

    if (i >= 0) {
        if (mine[i])
            free(environ[i]);
        environ[i] = s;
        mine[i] = true;
        return 0;
    }
    if (count + 2 > cap) {
        size_t c = cap * 2;
        char **v = realloc(owned, c * sizeof(char *));
        if (!v) {
            free(s);
            return ZK_ENOMEM;
        }
        owned = environ = v;
        bool *m = realloc(mine, c * sizeof(bool));
        if (!m) {
            free(s);
            return ZK_ENOMEM;
        }
        memset(m + cap, 0, (c - cap) * sizeof(bool));
        mine = m;
        cap = c;
    }
    environ[count] = s;
    mine[count] = true;
    environ[++count] = NULL;
    return 0;
}

int unsetenv(const char *name)
{
    int i = find(name, strlen(name));
    if (i < 0)
        return 0;
    if (!take_over())
        return ZK_ENOMEM;
    if (mine[i])
        free(environ[i]);
    for (size_t k = (size_t)i; k < count; k++) {
        environ[k] = environ[k + 1];
        mine[k] = k + 1 < cap ? mine[k + 1] : false;
    }
    count--;
    return 0;
}
