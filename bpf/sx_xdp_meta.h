/* SPDX-License-Identifier: GPL-2.0 */
/* AF_XDP RX metadata layout shared by XDP program and user space. */
#ifndef SX_XDP_META_H
#define SX_XDP_META_H

struct sx_xdp_meta {
    unsigned long long rx_timestamp;
    unsigned int valid;
    int err;
};

#endif
