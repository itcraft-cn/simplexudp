/*
 * xdp_echo.c - 纯 XDP_TX 反射程序的加载/挂载工具（微基准用）
 *
 * 用法：
 *   xdp_echo <ifname> <udp_port> <seconds> [obj] [log]
 *
 * 行为：加载 bpf/xdp_echo_kern.c 编译出的对象，写入目的端口，native 模式挂载，
 *       等待指定秒数后 detach。结果写入日志文件，不使用 stdout/stderr。
 */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <linux/if_link.h>
#include <net/if.h>

static FILE *g_log;

static void log_line(const char *fmt, ...) {
    va_list ap;
    if (g_log == NULL) {
        return;
    }
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

int main(int argc, char **argv) {
    const char *ifname;
    const char *objpath;
    const char *logpath;
    unsigned port;
    unsigned seconds;
    int ifindex;
    struct bpf_object *obj;
    struct bpf_program *prog;
    struct bpf_map *map;
    struct echo_config {
        unsigned short port;
        unsigned short pad;
    } cfg;
    __u32 key = 0;
    __u32 existing = 0;
    __u32 attached = 0;
    int prog_fd;
    int map_fd;
    int err;

    if (argc < 4) {
        return 2;
    }
    ifname = argv[1];
    port = (unsigned)atoi(argv[2]);
    seconds = (unsigned)atoi(argv[3]);
    objpath = argc > 4 ? argv[4] : "build/xdp_echo_kern.o";
    logpath = argc > 5 ? argv[5] : "xdp_echo.log";

    g_log = fopen(logpath, "w");
    if (g_log == NULL) {
        return 2;
    }

    ifindex = (int)if_nametoindex(ifname);
    if (ifindex == 0) {
        log_line("if_nametoindex(%s) failed", ifname);
        fclose(g_log);
        return 1;
    }

    if (bpf_xdp_query_id(ifindex, XDP_FLAGS_DRV_MODE, &existing) == 0 &&
        existing == 0) {
        (void)bpf_xdp_query_id(ifindex, XDP_FLAGS_SKB_MODE, &existing);
    }
    if (existing != 0) {
        log_line("xdp already attached on %s: id=%u", ifname, existing);
        fclose(g_log);
        return 1;
    }

    obj = bpf_object__open_file(objpath, NULL);
    err = libbpf_get_error(obj);
    if (err != 0) {
        log_line("open %s failed: %d", objpath, err);
        fclose(g_log);
        return 1;
    }
    if (bpf_object__load(obj) != 0) {
        log_line("load %s failed", objpath);
        bpf_object__close(obj);
        fclose(g_log);
        return 1;
    }
    prog = bpf_object__find_program_by_name(obj, "xdp_echo_prog");
    map = bpf_object__find_map_by_name(obj, "echo_config");
    if (prog == NULL || map == NULL) {
        log_line("prog/map not found in %s", objpath);
        bpf_object__close(obj);
        fclose(g_log);
        return 1;
    }
    prog_fd = bpf_program__fd(prog);
    map_fd = bpf_map__fd(map);
    memset(&cfg, 0, sizeof(cfg));
    cfg.port = (unsigned short)port;
    if (bpf_map_update_elem(map_fd, &key, &cfg, BPF_ANY) != 0) {
        log_line("update echo_config failed errno=%d", errno);
        bpf_object__close(obj);
        fclose(g_log);
        return 1;
    }

    if (bpf_xdp_attach(ifindex, prog_fd,
                       XDP_FLAGS_DRV_MODE | XDP_FLAGS_UPDATE_IF_NOEXIST,
                       NULL) != 0 ||
        bpf_xdp_query_id(ifindex, XDP_FLAGS_DRV_MODE, &attached) != 0 ||
        attached == 0) {
        log_line("attach failed on %s errno=%d", ifname, errno);
        bpf_object__close(obj);
        fclose(g_log);
        return 1;
    }
    log_line("attached if=%s port=%u prog_id=%u seconds=%u", ifname, port,
             attached, seconds);
    sleep(seconds);
    bpf_xdp_detach(ifindex, XDP_FLAGS_DRV_MODE, NULL);
    log_line("detached if=%s", ifname);
    bpf_object__close(obj);
    fclose(g_log);
    return 0;
}
