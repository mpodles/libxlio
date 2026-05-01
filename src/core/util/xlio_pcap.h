/*
 * SPDX-FileCopyrightText: NVIDIA CORPORATION & AFFILIATES
 * Copyright (c) 2021-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: GPL-2.0-only or BSD-2-Clause
 */

#ifndef XLIO_PCAP_H
#define XLIO_PCAP_H

/*
 * Ring-level raw pcap dumper.
 *
 * Taps rx_process_buffer() before any TCP/IP processing.  With HW kTLS the
 * NIC has already decrypted the payload, so the dump is plaintext.
 *
 * Runtime control (env vars, set before/at process start):
 *   XLIO_PCAP_DUMP=1              enable dumping
 *   XLIO_PCAP_DUMP_DIR=/tmp       output directory (default: /tmp)
 *
 * Output: $XLIO_PCAP_DUMP_DIR/xlio_ring_<pid>.pcap  (standard pcap, Ethernet link type)
 *
 * To remove all overhead in production builds, define XLIO_PCAP_DUMP_DISABLE.
 */

#ifndef XLIO_PCAP_DUMP_DISABLE

#include <cstddef>

/* Dump one raw Ethernet frame to the ring pcap file.
 * Gated at runtime by XLIO_PCAP_DUMP=1; no-op otherwise. */
void xlio_pcap_dump_frame(const void *data, size_t len);

#define XLIO_PCAP_DUMP_FRAME(data, len) xlio_pcap_dump_frame((data), (len))

#else /* XLIO_PCAP_DUMP_DISABLE */

#define XLIO_PCAP_DUMP_FRAME(data, len) do {} while (0)

#endif /* XLIO_PCAP_DUMP_DISABLE */

#endif /* XLIO_PCAP_H */
