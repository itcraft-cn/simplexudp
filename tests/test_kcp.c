/*
 * test_kcp.c - 可靠传输引擎单元测试（丢包 + 乱序）
 *
 * 方法：构造两个 KCP 控制块，用一个进程内的“有损/乱序”假 link 互连，
 *       发送 100 条消息，注入周期性丢包与相邻乱序，验证：
 *         - 全部按序到达；
 *         - 确有丢包发生且被重传恢复。
 */
#include "../src/sx_internal.h"

#include <string.h>
#include <unistd.h>

#include "sx_test.h"

#define CONV 0x1234abcdU
#define NMSG 100
#define QCAP 4096

typedef struct fake_pkt {
    uint8_t buf[1600];
    size_t len;
} fake_pkt_t;

typedef struct fakeio {
    sx_kcp_t *peer;
    fake_pkt_t q[QCAP];
    int head;
    int tail;
    uint64_t sent;
    uint64_t drop_mod;
    uint64_t dropped;
    int reorder;
} fakeio_t;

typedef struct recv_ctx {
    uint32_t expect;
    uint32_t got;
    uint32_t ooo;
} recv_ctx_t;

static sx_endpoint_t epA;
static sx_endpoint_t epB;
static sx_link_t linkA;
static sx_link_t linkB;
static sx_io_t ioA;
static sx_io_t ioB;
static fakeio_t fioA;
static fakeio_t fioB;
static recv_ctx_t ctxB;

static int fake_sendto(sx_io_t *io, uint32_t dst_ip, uint16_t dst_port,
                       const void *buf, size_t len) {
    fakeio_t *f = (fakeio_t *)io->priv;
    int next;
    (void)dst_ip;
    (void)dst_port;
    f->sent++;
    if (f->drop_mod != 0 && (f->sent % f->drop_mod) == 0) {
        f->dropped++;
        return SX_OK;
    }
    if (len > sizeof(f->q[0].buf)) {
        return SX_OK;
    }
    next = (f->tail + 1) % QCAP;
    if (next == f->head) {
        return SX_OK;
    }
    memcpy(f->q[f->tail].buf, buf, len);
    f->q[f->tail].len = len;
    f->tail = next;
    return SX_OK;
}

static int fake_recvfrom(sx_io_t *io, uint32_t *a, uint16_t *b, void *c,
                         size_t d, size_t *e) {
    (void)io; (void)a; (void)b; (void)c; (void)d; (void)e;
    return 0;
}

static int pump(fakeio_t *f) {
    int n = 0;
    while (f->head != f->tail) {
        fake_pkt_t *pk = &f->q[f->head];
        if (f->reorder) {
            int next = (f->head + 1) % QCAP;
            if (next != f->tail) {
                fake_pkt_t tmp = f->q[f->head];
                f->q[f->head] = f->q[next];
                f->q[next] = tmp;
            }
        }
        f->head = (f->head + 1) % QCAP;
        sx_kcp_input(f->peer, pk->buf, pk->len, sx_now_ms());
        n++;
    }
    return n;
}

static void on_recv(sx_endpoint_t *ep, sx_link_t *link, const void *data,
                    size_t len, void *user) {
    recv_ctx_t *c = (recv_ctx_t *)user;
    uint32_t v = 0;
    (void)ep;
    (void)link;
    if (len != sizeof(v)) {
        c->ooo++;
        return;
    }
    memcpy(&v, data, sizeof(v));
    if (v != c->expect) {
        c->ooo++;
    }
    c->expect = v + 1;
    c->got++;
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "test_kcp.log";
    sx_kcp_t *kA;
    sx_kcp_t *kB;
    uint32_t next = 0;
    int iter;

    T_BEGIN(path);
    memset(&epA, 0, sizeof(epA));
    memset(&epB, 0, sizeof(epB));
    memset(&linkA, 0, sizeof(linkA));
    memset(&linkB, 0, sizeof(linkB));
    memset(&ioA, 0, sizeof(ioA));
    memset(&ioB, 0, sizeof(ioB));
    memset(&fioA, 0, sizeof(fioA));
    memset(&fioB, 0, sizeof(fioB));
    memset(&ctxB, 0, sizeof(ctxB));

    epA.cfg.max_payload = 1400;
    epA.cfg.min_rto_ms = 20;
    epA.cfg.max_rto_ms = 500;
    epA.cfg.send_window = 256;
    epA.cfg.recv_window = 512;
    epA.cfg.keepalive_ms = 10000;
    epA.cfg.timeout_ms = 100000;
    epB = epA;
    epB.cfg.on_recv = on_recv;
    epB.cfg.user = &ctxB;

    epA.io = &ioA;
    epB.io = &ioB;
    ioA.name = "fakeA";
    ioA.priv = &fioA;
    ioA.sendto = fake_sendto;
    ioA.recvfrom = fake_recvfrom;
    ioB.name = "fakeB";
    ioB.priv = &fioB;
    ioB.sendto = fake_sendto;
    ioB.recvfrom = fake_recvfrom;

    linkA.ep = &epA;
    linkA.scope = SX_SCOPE_UNICAST;
    linkA.conv = CONV;
    linkA.addr.ip = 0x0a000001u;
    linkA.addr.port = 1000;
    linkB.ep = &epB;
    linkB.scope = SX_SCOPE_UNICAST;
    linkB.conv = CONV;
    linkB.addr.ip = 0x0a000002u;
    linkB.addr.port = 2000;

    kA = sx_kcp_create(&epA, &linkA, CONV, 256, 512);
    kB = sx_kcp_create(&epB, &linkB, CONV, 256, 512);
    T_CHECK(kA != NULL && kB != NULL, "kcp create");
    if (kA == NULL || kB == NULL) {
        T_END();
    }
    linkA.kcp = kA;
    linkB.kcp = kB;
    fioA.peer = kB;
    fioB.peer = kA;
    fioA.drop_mod = 5; /* 每 5 个丢 1 个 */
    fioA.reorder = 1;  /* 相邻乱序 */

    for (iter = 0; iter < 4000 && ctxB.got < NMSG; iter++) {
        int j;
        while (next < NMSG) {
            int64_t r = sx_kcp_send(kA, &next, sizeof(next));
            if (r < 0) {
                break;
            }
            next++;
        }
        sx_kcp_update(kA, sx_now_ms());
        sx_kcp_update(kB, sx_now_ms());
        for (j = 0; j < 16; j++) {
            int n = pump(&fioA);
            n += pump(&fioB);
            if (n == 0) {
                break;
            }
        }
        usleep(2000);
    }

    T_LOG("kA: una=%u nxt=%u cnt=%u cwnd=%u sndwnd=%u rto=%u srtt=%u",
          kA->snd_una, kA->snd_nxt, kA->snd_count, kA->cwnd, kA->snd_wnd,
          kA->rto_ms, kA->srtt);
    T_LOG("kB: rcv_nxt=%u rcv_cnt=%u rcvwnd=%u",
          kB->rcv_nxt, kB->rcv_count, kB->rcv_wnd);
    T_LOG("got=%u ooo=%u next=%u dropped=%llu retrans=%llu fast=%llu timeouts=%llu",
          ctxB.got, ctxB.ooo, next, (unsigned long long)fioA.dropped,
          (unsigned long long)epA.stats.retransmits,
          (unsigned long long)epA.stats.fast_retrans,
          (unsigned long long)epA.stats.timeouts);
    T_CHECK(ctxB.got == NMSG, "all messages delivered");
    T_CHECK(ctxB.ooo == 0, "delivered strictly in order");
    T_CHECK(fioA.dropped > 0, "loss actually injected");
    T_CHECK(epA.stats.retransmits > 0, "retransmission happened");

    sx_kcp_destroy(kA);
    sx_kcp_destroy(kB);
    T_END();
}
