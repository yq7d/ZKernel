#pragma once
// terminals: line discipline shared by the console and ptys. input is
// processed as it arrives, so ctrl+c works even when nobody is reading
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <zk/syscall.h>
#include "sched/sched.h"

#define TTY_LINE_MAX 256
#define TTY_IN_SIZE  2048

struct tty;
struct vnode;

struct tty_ops {
    // program output, may block
    void (*write)(struct tty *t, const char *s, size_t n);
    // echo from input processing, must not block (can be irq context)
    void (*echo)(struct tty *t, const char *s, size_t n);
};

struct tty {
    const struct tty_ops *ops;
    void *priv;
    uint32_t mode;
    char line[TTY_LINE_MAX];
    size_t line_len;
    char in[TTY_IN_SIZE];         // what readers get
    size_t in_head, in_count;
    int eofs;                     // ctrl+d on an empty line
    uint64_t fg;
    struct zk_winsize ws;
    bool hangup;
    struct waitq readers;
};

void tty_init(struct tty *t, const struct tty_ops *ops, void *priv);
// one byte of input, any context
void tty_input(struct tty *t, char c);
int64_t tty_read(struct tty *t, void *buf, size_t len);
int64_t tty_write(struct tty *t, const void *buf, size_t len);
int tty_ioctl(struct tty *t, unsigned long req, uint64_t arg);
int tty_poll(struct tty *t);
// the other end is gone: kills the foreground group, readers get eof
void tty_hangup(struct tty *t);

// the tty behind a console or pty vnode, NULL for anything else
struct tty *tty_of(struct vnode *v);

// the screen and keyboard
extern struct tty console_tty;
extern struct vnode tty_vnode;
extern struct vnode devtty_vnode;
void tty_console_init(void);
// while on, keyboard input goes to the console tty instead of the kernel shell
void tty_console_route(bool on);
bool tty_console_routed(void);

// ioctl args are user pointers, these check them. from proc/syscall.c
int copy_out(uint64_t dst, const void *src, size_t len);
int copy_in(void *dst, uint64_t src, size_t len);
