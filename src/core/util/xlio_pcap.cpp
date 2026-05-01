/*
 * SPDX-FileCopyrightText: NVIDIA CORPORATION & AFFILIATES
 * Copyright (c) 2021-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: GPL-2.0-only or BSD-2-Clause
 */

/*
 * Always compiled (no #ifdef guard) so the symbol is always present in
 * libxlio.so regardless of build flags.  The call sites in ring_slave.cpp
 * are conditionally compiled via the XLIO_PCAP_DUMP_FRAME macro in
 * xlio_pcap.h; define XLIO_PCAP_DUMP_DISABLE to make those call sites
 * zero-cost no-ops without touching this file.
 *
 * Runtime: set XLIO_PCAP_DUMP=1 (and optionally XLIO_PCAP_DUMP_DIR=<dir>)
 * before starting the process to enable actual capture.
 */

#ifndef XLIO_PCAP_DUMP_DISABLE

#include "xlio_pcap.h"
#include "vlogger/vlogger.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <sys/time.h>
#include <unistd.h>

/* pcap global file header (little-endian, microsecond timestamps) */
struct pcap_file_hdr {
    uint32_t magic_number;  /* 0xa1b2c3d4 — little-endian, usec resolution */
    uint16_t version_major; /* 2 */
    uint16_t version_minor; /* 4 */
    int32_t  thiszone;      /* 0 = UTC */
    uint32_t sigfigs;       /* 0 */
    uint32_t snaplen;       /* 65535 */
    uint32_t network;       /* 1 = LINKTYPE_ETHERNET */
};

/* pcap per-packet record header */
struct pcap_rec_hdr {
    uint32_t ts_sec;
    uint32_t ts_usec;
    uint32_t incl_len; /* bytes stored in file */
    uint32_t orig_len; /* bytes on the wire   */
};

static FILE            *g_pcap_file = nullptr;
static bool             g_enabled   = false;
static pthread_once_t   g_once      = PTHREAD_ONCE_INIT;
static pthread_mutex_t  g_lock      = PTHREAD_MUTEX_INITIALIZER;

static void pcap_close_atexit()
{
    if (g_pcap_file) {
        fflush(g_pcap_file);
        fclose(g_pcap_file);
        g_pcap_file = nullptr;
    }
}

static void pcap_init_once()
{
    const char *env = getenv("XLIO_PCAP_DUMP");
    vlog_printf(VLOG_WARNING, "xlio_pcap: init: XLIO_PCAP_DUMP=%s\n",
                env ? env : "(not set)");

    if (!env || !env[0] || env[0] == '0') {
        vlog_printf(VLOG_WARNING, "xlio_pcap: disabled (set XLIO_PCAP_DUMP=1 to enable)\n");
        return;
    }

    const char *dir = getenv("XLIO_PCAP_DUMP_DIR");
    if (!dir || !dir[0]) {
        dir = "/tmp";
    }

    char path[512];
    snprintf(path, sizeof(path), "%s/xlio_ring_%d.pcap", dir, (int)getpid());

    g_pcap_file = fopen(path, "wb");
    if (!g_pcap_file) {
        vlog_printf(VLOG_ERROR, "xlio_pcap: cannot open %s (errno=%d)\n", path, errno);
        return;
    }

    /* 1 MB stdio buffer to amortise syscall cost on the hot path */
    setvbuf(g_pcap_file, nullptr, _IOFBF, 1 << 20);

    const pcap_file_hdr hdr = {
        0xa1b2c3d4u, /* magic                */
        2u,          /* version_major        */
        4u,          /* version_minor        */
        0,           /* thiszone (UTC)       */
        0u,          /* sigfigs              */
        65535u,      /* snaplen              */
        1u,          /* LINKTYPE_ETHERNET    */
    };
    fwrite(&hdr, sizeof(hdr), 1, g_pcap_file);
    fflush(g_pcap_file);

    atexit(pcap_close_atexit);

    g_enabled = true;
    vlog_printf(VLOG_WARNING, "xlio_pcap: capturing ring traffic -> %s\n", path);
}

void xlio_pcap_dump_frame(const void *data, size_t len)
{
    pthread_once(&g_once, pcap_init_once);
    if (!g_enabled) {
        return;
    }

    struct timeval tv;
    gettimeofday(&tv, nullptr);

    const uint32_t incl_len = (uint32_t)(len <= 65535u ? len : 65535u);
    const pcap_rec_hdr rec = {
        (uint32_t)tv.tv_sec,
        (uint32_t)tv.tv_usec,
        incl_len,
        (uint32_t)len,
    };

    pthread_mutex_lock(&g_lock);
    fwrite(&rec, sizeof(rec), 1, g_pcap_file);
    fwrite(data, 1, incl_len, g_pcap_file);
    pthread_mutex_unlock(&g_lock);
}

#endif /* !XLIO_PCAP_DUMP_DISABLE */
