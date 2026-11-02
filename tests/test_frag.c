/*
 * test_frag.c - 大消息自动分片/重组单元测试
 *
 * 用两个 KCP 控制块经进程内 fake io 互连，发送一条 20000 字节消息，
 * 验证接收端只回调一次，且长度与内容完全一致。
 */
#include "../src/sx_internal.h"

#include <string.h>
#include <unistd.h>

#include "sx_test.h"

#define CONV 0x51f2a7u
#define BIG_LEN 20000u
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
} fakeio_t;

typedef struct recv_ctx {
    uint32_t got;
    size_t len;
    uint32_t sum;
    int bad;
} recv_ctx_t;

static sx_endpoint_t epA;
static sx_endpoint_t epB;
static sx_link_t linkA;
static sx_link_t linkB;
static sx_io_t ioA;
static sx_io_t ioB;
static fakeio_t fioA;
static fakeio_t fioB;
static recv_ctx_t ctx;

static int fake_sendto(sx_io_t *io, uint32_t dst_ip, uint16_t dst_port,
                       const void *buf, size_t len) {
    fakeio_t *f = (fakeio_t *)io->priv;
    int next;
    (void)dst_ip;
    (void)dst_port;
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
        f->head = (f->head + 1) % QCAP;
        sx_kcp_input(f->peer, pk->buf, pk->len, sx_now_ms());
        n++;
    }
    return n;
}

static void on_recv(sx_endpoint_t *ep, sx_link_t *link, const void *data,
                    size_t len, void *user) {
    recv_ctx_t *c = (recv_ctx_t *)user;
    const uint8_t *p = (const uint8_t *)data;
    size_t i;
    (void)ep;
    (void)link;
    c->got++;
    c->len = len;
    c->sum = 0;
    if (len != BIG_LEN) {
        c->bad = 1;
        return;
    }
    for (i = 0; i < len; i++) {
        c->sum += p[i];
        if (p[i] != (uint8_t)(i * 31u + 7u)) {
            c->bad = 1;
            return;
        }
    }
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "test_frag.log";
    sx_kcp_t *kA;
    sx_kcp_t *kB;
    uint8_t big[BIG_LEN];
    uint32_t i;
    int64_t r;

    T_BEGIN(path);
    memset(&epA, 0, sizeof(epA));
    memset(&epB, 0, sizeof(epB));
    memset(&linkA, 0, sizeof(linkA));
    memset(&linkB, 0, sizeof(linkB));
    memset(&ioA, 0, sizeof(ioA));
    memset(&ioB, 0, sizeof(ioB));
    memset(&fioA, 0, sizeof(fioA));
    memset(&fioB, 0, sizeof(fioB));
    memset(&ctx, 0, sizeof(ctx));

    epA.cfg.max_payload = 1400;
    epA.cfg.max_message = 64u * 1024u;
    epA.cfg.min_rto_ms = 20;
    epA.cfg.max_rto_ms = 500;
    epA.cfg.send_window = 256;
    epA.cfg.recv_window = 512;
    epA.cfg.keepalive_ms = 10000;
    epA.cfg.timeout_ms = 100000;
    epB = epA;
    epB.cfg.on_recv = on_recv;
    epB.cfg.user = &ctx;

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

    for (i = 0; i < BIG_LEN; i++) {
        big[i] = (uint8_t)(i * 31u + 7u);
    }
    r = sx_kcp_send_message(kA, big, BIG_LEN);
    T_LOG("send_message=%lld", (long long)r);
    T_CHECK(r >= 0, "fragmented send accepted");

    for (i = 0; i < 20000 && ctx.got == 0; i++) {
        int n = pump(&fioA);
        n += pump(&fioB);
        sx_kcp_update(kA, sx_now_ms());
        sx_kcp_update(kB, sx_now_ms());
        sx_ep_flush(&epB);
        if (n == 0) {
            usleep(1000);
        }
    }

    T_LOG("got=%u len=%zu bad=%d", ctx.got, ctx.len, ctx.bad);
    T_CHECK(ctx.got == 1, "exactly one reassembled message");
    T_CHECK(ctx.len == BIG_LEN, "reassembled length");
    T_CHECK(ctx.bad == 0, "reassembled content");

    sx_kcp_destroy(kA);
    sx_kcp_destroy(kB);
    T_END();
}
