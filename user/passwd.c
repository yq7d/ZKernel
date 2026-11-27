// passwd [user]. installed set-user-id root
#include "zk.h"

#define SHADOW "/etc/shadow"
// a copy that survives reboots, init puts it back
#define SAVED  "/usr/local/etc/shadow"

static int rewrite(const char *path, const char *user, const char *hash)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return fd;
    size_t cap = 4096, n = 0;
    char *out = malloc(cap);
    struct lreader *r = lr_open(fd);
    char *line;
    bool found = false;
    size_t ul = strlen(user);
    while (out && lr_getline(r, &line) >= 0) {
        char buf[512];
        if (strncmp(line, user, ul) == 0 && line[ul] == ':') {
            snprintf(buf, sizeof buf, "%s:%s\n", user, hash);
            found = true;
        } else {
            snprintf(buf, sizeof buf, "%s\n", line);
        }
        size_t bl = strlen(buf);
        if (n + bl > cap) {
            cap *= 2;
            out = realloc(out, cap);
            if (!out)
                break;
        }
        memcpy(out + n, buf, bl);
        n += bl;
    }
    lr_close(r);
    close(fd);
    if (!out)
        return ZK_ENOMEM;
    if (!found) {
        free(out);
        return ZK_ENOENT;
    }
    fd = open(path, O_WRONLY | O_TRUNC);
    if (fd < 0) {
        free(out);
        return fd;
    }
    long w = write_all(fd, out, n);
    close(fd);
    free(out);
    return w < 0 ? (int)w : 0;
}

int main(int argc, char **argv)
{
    struct zk_cred c;
    getcred(&c);
    struct passwd me;
    if (getpwuid(c.uid, &me) != 0) {
        dprintf(2, "passwd: you don't exist in /etc/passwd\n");
        return 1;
    }
    const char *user = argc > 1 ? argv[1] : me.name;
    if (c.uid != 0 && strcmp(user, me.name) != 0) {
        dprintf(2, "passwd: only root can change other users' passwords\n");
        return 1;
    }
    struct passwd pw;
    if (getpwnam(user, &pw) != 0) {
        dprintf(2, "passwd: unknown user %s\n", user);
        return 1;
    }
    if (c.uid != 0) {
        char old[128];
        read_password("Current password: ", old, sizeof old);
        bool ok = check_password(user, old);
        memset(old, 0, sizeof old);
        if (!ok) {
            dprintf(2, "passwd: wrong password\n");
            return 1;
        }
    }
    char a[128], b[128];
    read_password("New password: ", a, sizeof a);
    read_password("Retype new password: ", b, sizeof b);
    if (strcmp(a, b) != 0) {
        dprintf(2, "passwd: they don't match\n");
        return 1;
    }
    if (strlen(a) < 3) {
        dprintf(2, "passwd: too short\n");
        return 1;
    }
    uint8_t raw[8];
    getrandom(raw, sizeof raw);
    char salt[17], hash[128];
    for (int i = 0; i < 8; i++)
        snprintf(salt + i * 2, 3, "%02x", raw[i]);
    hash_password(a, salt, hash, sizeof hash);
    memset(a, 0, sizeof a);
    memset(b, 0, sizeof b);

    int err = rewrite(SHADOW, user, hash);
    if (err) {
        dprintf(2, "passwd: %s: %s\n", SHADOW, strerror(err));
        return 1;
    }
    // keep a copy where it survives a reboot, if there's persistent storage
    struct zk_stat st;
    if (stat("/usr/local/etc", &st) == 0) {
        int in = open(SHADOW, O_RDONLY), out = open(SAVED, O_WRONLY | O_CREAT | O_TRUNC);
        char buf[1024];
        long n;
        while (in >= 0 && out >= 0 && (n = read(in, buf, sizeof buf)) > 0)
            write_all(out, buf, (size_t)n);
        if (in >= 0)
            close(in);
        if (out >= 0) {
            close(out);
            chmod(SAVED, 0600);
        }
    }
    printf("passwd: password updated\n");
    return 0;
}
