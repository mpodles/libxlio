/*
 * SPDX-FileCopyrightText: NVIDIA CORPORATION & AFFILIATES
 * Copyright (c) 2021-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: GPL-2.0-only or BSD-2-Clause
 */

/**
 * @file xlio_extra.h
 * @brief XLIO Extended API for advanced features
 *
 * This file provides the extended XLIO API structure for accessing advanced features and the
 * XLIO Ultra API functions through a function pointer interface. This approach enables dynamic
 * API discovery when using LD_PRELOAD or dlopen/dlsym mechanisms.
 */

#ifndef XLIO_EXTRA_H
#define XLIO_EXTRA_H

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#include "xlio_types.h"

/* Magic value for xlio_get_api (NVDAXLIO) */
#define XLIO_MAGIC_NUMBER (0x4f494c584144564eULL)

/* Forward declaration. */
struct ibv_pd;

/*
 * XLIO Extended Socket API
 */

enum {
    XLIO_EXTRA_API_ADD_CONF_RULE = (1 << 3),
    XLIO_EXTRA_API_THREAD_OFFLOAD = (1 << 4),
    XLIO_EXTRA_API_DUMP_FD_STATS = (1 << 11),
    XLIO_EXTRA_API_XLIO_ULTRA = (1 << 13),
    /*
     * Zero-copy receive for BSD sockets with kTLS/UTLS-RX.
     * Indicates that xlio_recv_zc_fd() and xlio_recv_zc_release() are
     * populated in xlio_api_t.
     */
    XLIO_EXTRA_API_RECV_ZC = (1 << 14),
    /*
     * Convert an existing fd-based socket to an xlio_socket_t handle without
     * disrupting the connection.  Indicates that xlio_socket_from_fd() is
     * populated in xlio_api_t.
     *
     * This allows performing the TCP accept + TLS handshake on a regular fd,
     * then "upgrading" the socket to the Ultra API data path afterwards.
     *
     * NOTE: The returned handle shares the same underlying sockinfo_tcp as the
     * fd.  Do NOT call xlio_socket_destroy() on it; close the original fd to
     * tear down the connection.
     *
     * NOTE: For sockets with UTLS_TX active, xlio_socket_sendv(INLINE) bypasses
     * sockinfo_tcp_ops_tls::tcp_tx and therefore does NOT add TLS record
     * headers.  Use write_tls (SSL_write) for TLS-encrypted sends until a
     * TLS-aware express send path is added to sockinfo_tcp_ops_tls.
     */
    XLIO_EXTRA_API_SOCKET_FROM_FD = (1 << 15),
};

struct __attribute__((packed)) xlio_api_t {

    /*
     * Used to verify that API structure returned from xlio_get_api call is
     * compatible with current XLIO library version.
     */
    uint64_t magic;

    /*
     * Used to identify which methods were initialized by XLIO as part of xlio_get_api().
     * The value content is based on cap_mask bit field.
     * Order of fields in this structure should not be changed to keep abi compatibility.
     */
    uint64_t cap_mask;

    /*
     * Add a libxlio.conf rule to the top of the list.
     * This rule will not apply to existing sockets which already considered the conf rules.
     * (around connect/listen/send/recv ..)
     * @param config_line A char buffer with the exact format as defined in libxlio.conf, and should
     * end with '\0'.
     * @return 0 on success, or error code on failure.
     */
    int (*add_conf_rule)(const char *config_line);

    /*
     * Create sockets on pthread tid as offloaded/not-offloaded.
     * This does not affect existing sockets.
     * Offloaded sockets are still subject to libxlio.conf rules.
     * @param offload 1 for offloaded, 0 for not-offloaded.
     * @return 0 on success, or error code on failure.
     */
    int (*thread_offload)(int offload, pthread_t tid);

    /*
     * Dump fd statistics using the library logger.
     * @param fd to dump, 0 for all open fds.
     * @param log_level dumping level corresponding vlog_levels_t enum (vlogger.h).
     * @return 0 on success, or error code on failure.
     */
    int (*dump_fd_stats)(int fd, int log_level);

    /*
     * XLIO Ultra API.
     */
    int (*xlio_init_ex)(const struct xlio_init_attr *attr);
    int (*xlio_exit)(void);
    int (*xlio_poll_group_create)(const struct xlio_poll_group_attr *attr,
                                  xlio_poll_group_t *group_out);
    int (*xlio_poll_group_destroy)(xlio_poll_group_t group);
    void (*xlio_poll_group_poll)(xlio_poll_group_t group);
    int (*xlio_socket_create)(const struct xlio_socket_attr *attr, xlio_socket_t *sock_out);
    int (*xlio_socket_destroy)(xlio_socket_t sock);
    int (*xlio_socket_update)(xlio_socket_t sock, unsigned flags, uintptr_t userdata_sq);
    int (*xlio_socket_setsockopt)(xlio_socket_t sock, int level, int optname, const void *optval,
                                  socklen_t optlen);
    int (*xlio_socket_getsockname)(xlio_socket_t sock, struct sockaddr *addr, socklen_t *addrlen);
    int (*xlio_socket_getpeername)(xlio_socket_t sock, struct sockaddr *addr, socklen_t *addrlen);
    int (*xlio_socket_bind)(xlio_socket_t sock, const struct sockaddr *addr, socklen_t addrlen);
    int (*xlio_socket_connect)(xlio_socket_t sock, const struct sockaddr *to, socklen_t tolen);
    int (*xlio_socket_listen)(xlio_socket_t sock);
    struct ibv_pd *(*xlio_socket_get_pd)(xlio_socket_t sock);
    int (*xlio_socket_detach_group)(xlio_socket_t sock);
    int (*xlio_socket_attach_group)(xlio_socket_t sock, xlio_poll_group_t group);
    int (*xlio_socket_send)(xlio_socket_t sock, const void *data, size_t len,
                            const struct xlio_socket_send_attr *attr);
    int (*xlio_socket_sendv)(xlio_socket_t sock, const struct iovec *iov, unsigned iovcnt,
                             const struct xlio_socket_send_attr *attr);
    void (*xlio_poll_group_flush)(xlio_poll_group_t group);
    void (*xlio_socket_flush)(xlio_socket_t sock);
    void (*xlio_socket_buf_free)(xlio_socket_t sock, struct xlio_buf *buf);
    void (*xlio_poll_group_buf_free)(xlio_poll_group_t group, struct xlio_buf *buf);
    uint32_t (*xlio_socket_buf_get_mkey)(struct xlio_buf *buf);
    struct ibv_pd *(*xlio_socket_buf_get_pd)(struct xlio_buf *buf);
    void *(*xlio_socket_buf_get_data)(struct xlio_buf *buf);
    size_t (*xlio_socket_buf_get_size)(struct xlio_buf *buf);

    /*
     * Zero-copy receive for BSD sockets with kTLS/UTLS-RX (cap: XLIO_EXTRA_API_RECV_ZC).
     *
     * xlio_recv_zc_fd() - obtain direct pointers into XLIO DMA receive buffers.
     *
     *   fd       - a connected TCP socket fd managed by XLIO that has completed
     *              a kTLS handshake with UTLS_RX enabled.
     *   segs     - caller-allocated array of xlio_zc_seg; filled on success.
     *   max_segs - capacity of segs[].
     *
     *   Returns the number of segments filled (>= 1), or -1 on error:
     *     errno = EAGAIN   - no data available right now (non-blocking).
     *     errno = ENOTSUP  - fd is not an XLIO-managed UTLS_RX socket.
     *     errno = ENODATA  - the next queued TLS record has tls_type != 0x17;
     *                        caller must process it via SSL_read before retrying.
     *
     *   Segments are consecutive buffers from the socket's ready-list, stopping
     *   at the first buffer whose tls_type differs from XLIO_TLS_RT_APPLICATION_DATA.
     *   Each returned buffer has its reference count bumped and must be released
     *   with xlio_recv_zc_release().
     *
     *   TCP flow-control (tcp_recved / ACK) is handled internally.
     *
     * xlio_recv_zc_release() - return one segment buffer back to XLIO.
     *
     *   Safe to call immediately after nghttp2_session_mem_recv2() returns,
     *   because that function processes all callbacks synchronously before
     *   returning; by that point the DMA buffer contents have been consumed.
     */
    int  (*xlio_recv_zc_fd)(int fd, struct xlio_zc_seg *segs, int max_segs);
    void (*xlio_recv_zc_release)(struct xlio_buf *buf);

    /*
     * Obtain an xlio_socket_t handle for an existing file-descriptor-based
     * TCP connection (cap: XLIO_EXTRA_API_SOCKET_FROM_FD).
     *
     * Returns a non-zero xlio_socket_t on success, or 0 if the fd is not an
     * XLIO-managed TCP socket (errno = ENOTSUP).
     *
     * The fd remains valid and the connection is not disrupted.  The returned
     * handle can be used with xlio_socket_sendv / xlio_socket_flush.
     * It must NOT be passed to xlio_socket_destroy().
     */
    xlio_socket_t (*xlio_socket_from_fd)(int fd);
};

/*
 * Retrieve XLIO extended API.
 * This function can be called as an alternative to getsockopt() call
 * when library is preloaded using LD_PRELOAD
 * getsockopt() call should be used in case application loads library
 * using dlopen()/dlsym().
 *
 * @return Pointer to the XLIO Extended Socket API, or NULL if XLIO not found.
 */
static inline struct xlio_api_t *xlio_get_api()
{
    struct xlio_api_t *api_ptr = NULL;
    socklen_t len = sizeof(api_ptr);

    /* coverity[negative_returns] */
    int err = getsockopt(-2, SOL_SOCKET, SO_XLIO_GET_API, &api_ptr, &len);
    if (err < 0) {
        return NULL;
    }
    if (len < sizeof(struct xlio_api_t *) || api_ptr == NULL ||
        api_ptr->magic != XLIO_MAGIC_NUMBER) {
        return NULL;
    }
    return api_ptr;
}

#endif /* XLIO_EXTRA_H */
