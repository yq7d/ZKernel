#pragma once
#include "net/net.h"

struct tcb;

// starts connecting, the syn is on its way when this returns
struct tcb *tcp_open(uint32_t dst, uint16_t dport, uint16_t lport, int *err);
int tcp_wait_connected(struct tcb *t, bool nonblock);
// a pending error, 0 if none (SO_ERROR)
int tcp_error(struct tcb *t);
int64_t tcp_send(struct tcb *t, const void *buf, size_t len, bool nonblock);
// 0 once the peer closed its side
int64_t tcp_recv(struct tcb *t, void *buf, size_t len, bool nonblock);
int tcp_poll(struct tcb *t);
void tcp_shutdown(struct tcb *t);
// the socket is closed, the connection finishes on its own and gets freed
void tcp_release(struct tcb *t);
void tcp_addrs(struct tcb *t, uint32_t *lip, uint16_t *lport, uint32_t *rip, uint16_t *rport);
bool tcp_port_used(uint16_t port);
