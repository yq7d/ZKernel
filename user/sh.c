// zkernel shell: pipelines, < > >> redirects, & background jobs, quotes,
// $VAR expansion, NAME=value assignments and PATH lookup
#include "zk.h"

#define LINE_MAX   512
#define MAX_TOKENS 64
#define MAX_STAGES 8
#define MAX_JOBS   16

enum { T_WORD, T_PIPE, T_IN, T_OUT, T_APPEND, T_BG, T_SEMI, T_AND, T_OR };

static bool is_name_char(char c, bool first)
{
    return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (!first && c >= '0' && c <= '9');
}

// NAME=value
static bool is_assignment(const char *s)
{
    if (!is_name_char(s[0], true))
        return false;
    const char *p = s + 1;
    while (is_name_char(*p, false))
        p++;
    return *p == '=';
}

struct token {
    int type;
    char *text;
};

struct stage {
    char *argv[MAX_TOKENS + 1];
    int argc;
    char *assign[MAX_TOKENS];
    int nassign;
    const char *in, *out;
    bool append;
};

static int last_status;
static bool interactive;
static long jobs[MAX_JOBS];

static void err(const char *what, int e)
{
    dprintf(2, "sh: %s: %s\n", what, strerror(e));
}

// splits line in place. words keep quoted spaces, $? expands to the last status
static int tokenize(char *line, struct token *toks, char *store, size_t store_size)
{
    int n = 0;
    size_t used = 0;
    char *p = line;

    for (;;) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p || *p == '#')
            break;
        if (n == MAX_TOKENS) {
            dprintf(2, "sh: too many words\n");
            return -1;
        }

        if ((p[0] == '&' && p[1] == '&') || (p[0] == '|' && p[1] == '|')) {
            toks[n++].type = p[0] == '&' ? T_AND : T_OR;
            p += 2;
            continue;
        }
        if (*p == ';') {
            toks[n++].type = T_SEMI;
            p++;
            continue;
        }
        if (*p == '|' || *p == '<' || *p == '&') {
            toks[n++].type = *p == '|' ? T_PIPE : *p == '<' ? T_IN : T_BG;
            p++;
            continue;
        }
        if (*p == '>') {
            if (p[1] == '>') {
                toks[n++].type = T_APPEND;
                p += 2;
            } else {
                toks[n++].type = T_OUT;
                p++;
            }
            continue;
        }

        char *w = store + used;
        size_t len = 0;
        char quote = 0;
        while (*p && (quote || (*p != ' ' && *p != '\t' && !strchr("|<>&;", *p)))) {
            char c = *p++;
            if (quote && c == quote) {
                quote = 0;
                continue;
            }
            if (!quote && (c == '"' || c == '\'')) {
                quote = c;
                continue;
            }
            if (quote != '\'' && c == '$') {
                char num[16];
                const char *val = NULL;
                if (*p == '?') {
                    p++;
                    snprintf(num, sizeof num, "%d", last_status);
                    val = num;
                } else if (*p == '$') {
                    p++;
                    snprintf(num, sizeof num, "%ld", getpid());
                    val = num;
                } else if (*p == '{' || is_name_char(*p, true)) {
                    char name[64];
                    size_t n = 0;
                    bool brace = *p == '{';
                    if (brace)
                        p++;
                    while (is_name_char(*p, n == 0) && n + 1 < sizeof name)
                        name[n++] = *p++;
                    name[n] = '\0';
                    if (brace && *p == '}')
                        p++;
                    val = getenv(name);
                    if (!val)
                        val = "";
                }
                if (!val) {
                    if (used + len + 1 < store_size)
                        w[len++] = '$';
                    continue;
                }
                for (; *val && used + len + 1 < store_size; val++)
                    w[len++] = *val;
                continue;
            }
            if (used + len + 1 < store_size)
                w[len++] = c;
        }
        if (quote) {
            dprintf(2, "sh: unterminated quote\n");
            return -1;
        }
        w[len] = '\0';
        used += len + 1;
        toks[n].type = T_WORD;
        toks[n++].text = w;
    }
    return n;
}

// returns the stage count, -1 on a syntax error
static int parse(struct token *toks, int n, struct stage *st, bool *bg)
{
    int ns = 0;
    *bg = false;
    memset(&st[0], 0, sizeof st[0]);

    for (int i = 0; i < n; i++) {
        struct stage *s = &st[ns];
        switch (toks[i].type) {
        case T_WORD:
            if (s->argc == 0 && is_assignment(toks[i].text))
                s->assign[s->nassign++] = toks[i].text;
            else
                s->argv[s->argc++] = toks[i].text;
            break;
        case T_PIPE:
            if (!s->argc || ns + 1 == MAX_STAGES)
                goto syntax;
            ns++;
            memset(&st[ns], 0, sizeof st[ns]);
            break;
        case T_IN:
        case T_OUT:
        case T_APPEND:
            if (i + 1 == n || toks[i + 1].type != T_WORD)
                goto syntax;
            if (toks[i].type == T_IN) {
                s->in = toks[++i].text;
            } else {
                s->append = toks[i].type == T_APPEND;
                s->out = toks[++i].text;
            }
            break;
        case T_BG:
            if (i + 1 != n)
                goto syntax;
            *bg = true;
            break;
        }
    }
    if (!st[ns].argc) {
        // a line of just assignments sets shell variables
        if (ns == 0 && !st[0].in && !st[0].out && !*bg) {
            for (int i = 0; i < st[0].nassign; i++) {
                char *eq = strchr(st[0].assign[i], '=');
                *eq = '\0';
                setenv(st[0].assign[i], eq + 1, 1);
            }
            last_status = 0;
            return 0;
        }
        goto syntax;
    }
    for (int i = 0; i <= ns; i++)
        st[i].argv[st[i].argc] = NULL;
    return ns + 1;

syntax:
    dprintf(2, "sh: syntax error\n");
    return -1;
}

static bool is_file(const char *path)
{
    struct zk_stat st;
    return stat(path, &st) == 0 && st.type == ZK_TYPE_FILE;
}

// names without a slash are searched for in $PATH
static bool resolve(const char *name, char *out)
{
    if (strchr(name, '/')) {
        strlcpy(out, name, ZK_PATH_MAX);
        return is_file(out);
    }
    const char *path = getenv("PATH");
    if (!path)
        path = "/bin";
    while (*path) {
        const char *end = strchr(path, ':');
        size_t n = end ? (size_t)(end - path) : strlen(path);
        if (n && n + 1 + strlen(name) < ZK_PATH_MAX) {
            memcpy(out, path, n);
            out[n] = '/';
            strlcpy(out + n + 1, name, ZK_PATH_MAX - n - 1);
            if (is_file(out))
                return true;
        }
        path += n;
        if (*path == ':')
            path++;
    }
    return false;
}

// environ plus the stage's NAME=value prefixes
static char **stage_env(struct stage *s, char **buf, size_t max)
{
    size_t n = 0;
    for (int i = 0; environ && environ[i] && n + 1 < max; i++) {
        bool overridden = false;
        size_t nl = strchr(environ[i], '=') ? (size_t)(strchr(environ[i], '=') - environ[i]) : 0;
        for (int k = 0; k < s->nassign; k++)
            if (strncmp(s->assign[k], environ[i], nl + 1) == 0)
                overridden = true;
        if (!overridden)
            buf[n++] = environ[i];
    }
    for (int k = 0; k < s->nassign && n + 1 < max; k++)
        buf[n++] = s->assign[k];
    buf[n] = NULL;
    return buf;
}

static void reap_jobs(void)
{
    int status;
    long pid;
    while ((pid = wait(-1, &status, WNOHANG)) > 0) {
        for (int i = 0; i < MAX_JOBS; i++) {
            if (jobs[i] == pid) {
                jobs[i] = 0;
                if (interactive)
                    printf("[%ld] done, status %d\n", pid, status);
            }
        }
    }
}

static void run_file(const char *path);

static void help(void)
{
    printf("builtins: cd [dir], pwd, export NAME=value, unset NAME, source file,\n"
           "          exit [code], help\n"
           "programs live in /bin, try ls /bin\n"
           "  a | b       pipe a into b\n"
           "  a > f       write to f, >> appends, < f reads from f\n"
           "  a &         run in the background\n"
           "  a ; b       one after the other, a && b only if a worked, a || b if it didn't\n"
           "  $NAME       a variable, $? is the last status, $$ the shell's pid\n"
           "  NAME=value  set a variable, or just for one command: NAME=value cmd\n"
           "  ctrl+c      kill the foreground job, ctrl+d at the prompt exits\n");
}

// true if it was a builtin
static bool builtin(struct stage *s)
{
    char **argv = s->argv;
    if (strcmp(argv[0], "cd") == 0) {
        const char *dir = s->argc > 1 ? argv[1] : getenv("HOME");
        if (!dir)
            dir = "/";
        int e = chdir(dir);
        if (e)
            err(dir, e);
        last_status = e ? 1 : 0;
        return true;
    }
    if (strcmp(argv[0], "export") == 0) {
        last_status = 0;
        for (int i = 1; i < s->argc; i++) {
            char *eq = strchr(argv[i], '=');
            if (!eq)
                continue;    // every variable is exported already
            *eq = '\0';
            if (setenv(argv[i], eq + 1, 1) < 0) {
                dprintf(2, "sh: export: bad name %s\n", argv[i]);
                last_status = 1;
            }
        }
        return true;
    }
    if (strcmp(argv[0], "unset") == 0) {
        for (int i = 1; i < s->argc; i++)
            unsetenv(argv[i]);
        last_status = 0;
        return true;
    }
    if (strcmp(argv[0], "source") == 0 || strcmp(argv[0], ".") == 0) {
        if (s->argc < 2) {
            dprintf(2, "sh: source: need a file\n");
            last_status = 1;
            return true;
        }
        run_file(argv[1]);
        return true;
    }
    if (strcmp(argv[0], "pwd") == 0) {
        char cwd[ZK_PATH_MAX];
        if (getcwd(cwd, sizeof cwd) >= 0)
            printf("%s\n", cwd);
        last_status = 0;
        return true;
    }
    if (strcmp(argv[0], "exit") == 0)
        exit(s->argc > 1 ? atoi(argv[1]) : last_status);
    if (strcmp(argv[0], "help") == 0) {
        help();
        last_status = 0;
        return true;
    }
    return false;
}

static void run(struct stage *st, int ns, bool bg)
{
    if (ns == 1 && !st[0].in && !st[0].out && builtin(&st[0]))
        return;

    long pids[MAX_STAGES];
    int spawned = 0;
    long pgid = 0;
    int prev_rd = -1;
    bool failed = false;

    for (int i = 0; i < ns && !failed; i++) {
        struct stage *s = &st[i];
        int in = prev_rd >= 0 ? prev_rd : 0;
        int out = 1;
        int pfd[2] = { -1, -1 };
        int in_file = -1, out_file = -1;

        if (i + 1 < ns) {
            int e = pipe(pfd);
            if (e) {
                err("pipe", e);
                failed = true;
                break;
            }
            out = pfd[1];
        }
        if (s->in) {
            in_file = open(s->in, O_RDONLY);
            if (in_file < 0) {
                err(s->in, in_file);
                failed = true;
            }
            in = in_file;
        }
        if (s->out && !failed) {
            out_file = open(s->out, O_WRONLY | O_CREAT | (s->append ? O_APPEND : O_TRUNC));
            if (out_file < 0) {
                err(s->out, out_file);
                failed = true;
            }
            out = out_file;
        }

        char path[ZK_PATH_MAX];
        if (!failed) {
            if (!resolve(s->argv[0], path)) {
                dprintf(2, "sh: %s: command not found\n", s->argv[0]);
                failed = true;
            } else {
                struct zk_spawn opts = { { in, out, 2 }, (uint64_t)pgid };
                char *envbuf[128];
                long pid = spawnve(path, s->argv, &opts, stage_env(s, envbuf, 128));
                if (pid < 0) {
                    err(s->argv[0], (int)pid);
                    failed = true;
                } else {
                    pids[spawned++] = pid;
                    if (!pgid)
                        pgid = pid;
                }
            }
        }

        // the children hold their own references now
        if (prev_rd >= 0)
            close(prev_rd);
        if (in_file >= 0)
            close(in_file);
        if (out_file >= 0)
            close(out_file);
        if (pfd[1] >= 0)
            close(pfd[1]);
        prev_rd = pfd[0];
    }
    if (prev_rd >= 0)
        close(prev_rd);

    if (spawned == 0) {
        last_status = 127;
        return;
    }

    if (bg) {
        for (int i = 0; i < spawned; i++) {
            for (int j = 0; j < MAX_JOBS; j++) {
                if (!jobs[j]) {
                    jobs[j] = pids[i];
                    break;
                }
            }
        }
        printf("[%ld]\n", pids[spawned - 1]);
        last_status = 0;
        return;
    }

    tty_setfg(pgid);
    int status = 0;
    for (int i = 0; i < spawned; i++)
        wait(pids[i], &status, 0);
    tty_setfg(0);
    last_status = failed ? 1 : status;
    if (status == ZK_EXIT_KILLED && interactive)
        printf("[killed]\n");
}

struct reader {
    int fd;
    size_t pos, len;
    char buf[512];
};

// one line without the newline. -1 at eof
static long get_line(struct reader *r, char *buf, size_t size)
{
    size_t n = 0;
    for (;;) {
        if (r->pos == r->len) {
            long got = read(r->fd, r->buf, sizeof r->buf);
            if (got <= 0) {
                buf[n] = '\0';
                return n ? (long)n : -1;
            }
            r->pos = 0;
            r->len = (size_t)got;
        }
        char c = r->buf[r->pos++];
        if (c == '\n')
            break;
        if (n + 1 < size)
            buf[n++] = c;
    }
    buf[n] = '\0';
    return (long)n;
}

static void run_line(char *line)
{
    static struct token toks[MAX_TOKENS];
    static struct stage st[MAX_STAGES];
    static char store[LINE_MAX * 2];

    int n = tokenize(line, toks, store, sizeof store);
    if (n <= 0)
        return;

    // a ; b, a && b, a || b
    int start = 0, prev_sep = T_SEMI;
    while (start < n) {
        int end = start;
        while (end < n && toks[end].type != T_SEMI && toks[end].type != T_AND &&
               toks[end].type != T_OR && toks[end].type != T_BG)
            end++;
        // "a & b" backgrounds a and goes on with b
        bool amp = end < n && toks[end].type == T_BG;
        if (amp)
            end++;
        bool skip = (prev_sep == T_AND && last_status != 0) ||
                    (prev_sep == T_OR && last_status == 0);
        if (end == start && end < n) {
            dprintf(2, "sh: syntax error\n");
            last_status = 2;
            return;
        }
        if (!skip && end > start) {
            bool bg;
            int ns = parse(toks + start, end - start, st, &bg);
            if (ns < 0) {
                last_status = 2;
                return;
            }
            if (ns > 0)
                run(st, ns, bg);
        }
        if (end == n)
            break;
        if (amp) {
            prev_sep = T_SEMI;
            start = end;
            continue;
        }
        prev_sep = toks[end].type;
        start = end + 1;
    }
}

static int depth;

static void run_file(const char *path)
{
    if (depth >= 8) {
        dprintf(2, "sh: %s: nested too deep\n", path);
        last_status = 1;
        return;
    }
    struct reader *r = malloc(sizeof(*r));
    char *line = malloc(LINE_MAX);
    if (!r || !line) {
        free(r);
        free(line);
        return;
    }
    r->fd = open(path, O_RDONLY);
    r->pos = r->len = 0;
    if (r->fd < 0) {
        err(path, r->fd);
        last_status = 1;
    } else {
        depth++;
        while (get_line(r, line, LINE_MAX) >= 0)
            run_line(line);
        depth--;
        close(r->fd);
    }
    free(line);
    free(r);
}

int main(int argc, char **argv)
{
    static char line[LINE_MAX];
    static struct reader in;

    if (argc > 2 && strcmp(argv[1], "-c") == 0) {
        strlcpy(line, argv[2], sizeof line);
        run_line(line);
        return last_status;
    }
    if (argc > 1) {
        run_file(argv[1]);
        return last_status;
    }

    struct zk_stat st;
    interactive = fstat(0, &st) == 0 && st.type == ZK_TYPE_DEV;
    if (interactive && stat("/etc/profile", &st) == 0)
        run_file("/etc/profile");
    if (interactive) {
        int fd = open("/etc/motd", O_RDONLY);
        if (fd >= 0) {
            char buf[512];
            long n;
            while ((n = read(fd, buf, sizeof buf)) > 0)
                write_all(1, buf, (size_t)n);
            close(fd);
        }
    }

    for (;;) {
        reap_jobs();
        if (interactive) {
            char cwd[ZK_PATH_MAX];
            if (getcwd(cwd, sizeof cwd) < 0)
                strlcpy(cwd, "?", sizeof cwd);
            printf("\e[1;32mzk\e[0m:\e[1;34m%s\e[0m%s ", cwd, last_status ? "\e[31m$\e[0m" : "$");
        }
        if (get_line(&in, line, sizeof line) < 0)
            break;
        run_line(line);
    }
    if (interactive)
        printf("exit\n");
    return last_status;
}
