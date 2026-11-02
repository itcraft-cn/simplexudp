/*
 * test_cc.c - 拥塞控制开关单元测试
 *
 * 验证：
 *   1) SX_CC_OFF：初始窗口为 send_window，超时只退避 RTO，不降窗；
 *   2) SX_CC_ON：初始窗口按慢启动从 16 起步，超时降为 1。
 *
 * 说明：直接驱动 KCP 控制块，底层用不投递的 fake io，故意制造 RTO 超时。
 */
#include "../src/sx_internal.h"

#include <string.h>
#include <unistd.h>

#include "sx_test.h"

#define CONV 0x0cc0ffeeu

typedef struct fakeio {
    uint64_t sent;
} fakeio_t;

static int fake_sendto(sx_io_t *io, uint32_t dip, uint16_t dport,
                       const void *buf, size_t len) {
    fakeio_t *f = (fakeio_t *)io->priv;
    (void)dip;
    (void)dport;
    (void)buf;
    (void)len;
    f->sent++;
    return SX_OK;
}

static int fake_recvfrom(sx_io_t *io, uint32_t *a, uint16_t *b, void *c,
                         size_t d, size_t *e, sx_pkt_info_t *info) {
    (void)io; (void)a; (void)b; (void)c; (void)d; (void)e; (void)info;
    return 0;
}

static void setup(sx_endpoint_t *ep, sx_link_t *link, sx_io_t *io,
                  fakeio_t *fio, uint32_t send_window, sx_cc_mode_t cc) {
    memset(ep, 0, sizeof(*ep));
    memset(link, 0, sizeof(*link));
    memset(io, 0, sizeof(*io));
    memset(fio, 0, sizeof(*fio));

    ep->cfg.max_payload = 1400;
    ep->cfg.send_window = send_window;
    ep->cfg.recv_window = send_window * 2;
    ep->cfg.min_rto_ms = 20;
    ep->cfg.max_rto_ms = 1000;
    ep->cfg.cc_mode = cc;
    ep->cfg.rto_backoff = 1;

    ep->io = io;
    io->name = "fake";
    io->priv = fio;
    io->sendto = fake_sendto;
    io->recvfrom = fake_recvfrom;

    link->ep = ep;
    link->scope = SX_SCOPE_UNICAST;
    link->conv = CONV;
    link->addr.ip = 0x0a000002u;
    link->addr.port = 9000;
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "test_cc.log";
    sx_endpoint_t ep1;
    sx_endpoint_t ep2;
    sx_endpoint_t ep3;
    sx_link_t link1;
    sx_link_t link2;
    sx_link_t link3;
    sx_io_t io1;
    sx_io_t io2;
    sx_io_t io3;
    fakeio_t fio1;
    fakeio_t fio2;
    fakeio_t fio3;
    sx_kcp_t *off;
    sx_kcp_t *on;
    sx_kcp_t *fixed;
    uint32_t payload = 0x12345678u;

    T_BEGIN(path);

    /* SX_CC_OFF：窗口一次性拉满，超时不回退。 */
    setup(&ep1, &link1, &io1, &fio1, 256, SX_CC_OFF);
    off = sx_kcp_create(&ep1, &link1, CONV, 256, 512);
    T_CHECK(off != NULL, "cc off kcp create");
    if (off == NULL) {
        T_END();
    }
    link1.kcp = off;
    T_CHECK(off->cc_enabled == 0, "cc disabled flag");
    T_CHECK(off->cwnd == 256, "cc off cwnd equals send_window");
    T_CHECK(sx_kcp_send(off, &payload, sizeof(payload)) >= 0, "cc off send");
    usleep(30000); /* 超过 min_rto=20ms，触发一次超时 */
    sx_kcp_update(off, sx_now_ms());
    T_LOG("cc off: cwnd=%u rto=%u timeouts=%llu", off->cwnd, off->rto_ms,
          (unsigned long long)ep1.stats.timeouts);
    T_CHECK(ep1.stats.timeouts > 0, "cc off timeout happened");
    T_CHECK(off->cwnd == 256, "cc off cwnd not collapsed");
    T_CHECK(off->rto_ms > 20, "cc off rto backed off");

    /* SX_CC_ON：初始 16，超时降为 1。 */
    setup(&ep2, &link2, &io2, &fio2, 256, SX_CC_ON);
    on = sx_kcp_create(&ep2, &link2, CONV, 256, 512);
    T_CHECK(on != NULL, "cc on kcp create");
    if (on == NULL) {
        sx_kcp_destroy(off);
        T_END();
    }
    link2.kcp = on;
    T_CHECK(on->cc_enabled == 1, "cc enabled flag");
    T_CHECK(on->cwnd == 16, "cc on initial cwnd");
    T_CHECK(sx_kcp_send(on, &payload, sizeof(payload)) >= 0, "cc on send");
    usleep(30000);
    sx_kcp_update(on, sx_now_ms());
    T_LOG("cc on: cwnd=%u rto=%u timeouts=%llu", on->cwnd, on->rto_ms,
          (unsigned long long)ep2.stats.timeouts);
    T_CHECK(ep2.stats.timeouts > 0, "cc on timeout happened");
    T_CHECK(on->cwnd == 1, "cc on cwnd collapsed");

    /* rto_backoff=0：超时只重传，不把 RTO 翻倍。 */
    setup(&ep3, &link3, &io3, &fio3, 256, SX_CC_OFF);
    ep3.cfg.rto_backoff = 0;
    fixed = sx_kcp_create(&ep3, &link3, CONV, 256, 512);
    T_CHECK(fixed != NULL, "fixed rto kcp create");
    if (fixed != NULL) {
        link3.kcp = fixed;
        T_CHECK(fixed->rto_backoff == 0, "rto backoff disabled flag");
        T_CHECK(sx_kcp_send(fixed, &payload, sizeof(payload)) >= 0,
                "fixed rto send");
        usleep(30000);
        sx_kcp_update(fixed, sx_now_ms());
        T_LOG("fixed rto: cwnd=%u rto=%u timeouts=%llu", fixed->cwnd,
              fixed->rto_ms, (unsigned long long)ep3.stats.timeouts);
        T_CHECK(ep3.stats.timeouts > 0, "fixed rto timeout happened");
        T_CHECK(fixed->rto_ms == 20, "fixed rto not backed off");
        sx_kcp_destroy(fixed);
    }

    sx_kcp_destroy(off);
    sx_kcp_destroy(on);
    T_END();
}
