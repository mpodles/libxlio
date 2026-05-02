/*
 * SPDX-FileCopyrightText: NVIDIA CORPORATION & AFFILIATES
 * Copyright (c) 2023-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: GPL-2.0-only or BSD-2-Clause
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <cstdio>
#include <util/sys_vars.h>
#include <util/libxlio.h>
#include <vlogger/vlogger.h>
#include <dev/buffer_pool.h>
#include <event/event_handler_manager_local.h>
#include <event/poll_group.h>
#include <sock/sockinfo.h>
#include <sock/sockinfo_tcp.h>
#include <sock/sockinfo_udp.h>
#include <sock/fd_collection.h>

#include "sock/sock-extra.h"
#include "xlio.h"

#define MODULE_NAME "extra:"

#define SET_EXTRA_API(__dst, __func, __mask)                                                       \
    do {                                                                                           \
        xlio_api.__dst = __func;                                                                   \
        xlio_api.cap_mask |= __mask;                                                               \
    } while (0);

//-----------------------------------------------------------------------------
// extended API functions
//-----------------------------------------------------------------------------

extern "C" int xlio_add_conf_rule(const char *config_line)
{
    int ret = __xlio_parse_config_line(config_line);

    if (*g_p_vlogger_level >= VLOG_DEBUG) {
        __xlio_print_conf_file(__instance_list);
    }

    return ret;
}

extern "C" int xlio_thread_offload(int offload, pthread_t tid)
{
    if (g_p_fd_collection) {
        g_p_fd_collection->offloading_rule_change_thread(offload, tid);
    } else {
        return -1;
    }

    return 0;
}

extern "C" int xlio_dump_fd_stats(int fd, int log_level)
{
    if (g_p_fd_collection) {
        g_p_fd_collection->statistics_print(fd, log_level::from_int(log_level));
        return 0;
    }
    return -1;
}

struct xlio_api_t *extra_api()
{
    // xlio_api is zerod-out by linker
    static struct xlio_api_t xlio_api;

    if (xlio_api.magic != XLIO_MAGIC_NUMBER) {
        memset(&xlio_api, 0, sizeof(struct xlio_api_t));
        xlio_api.magic = XLIO_MAGIC_NUMBER;

        SET_EXTRA_API(add_conf_rule, xlio_add_conf_rule, XLIO_EXTRA_API_ADD_CONF_RULE);
        SET_EXTRA_API(thread_offload, xlio_thread_offload, XLIO_EXTRA_API_THREAD_OFFLOAD);
        SET_EXTRA_API(dump_fd_stats, xlio_dump_fd_stats, XLIO_EXTRA_API_DUMP_FD_STATS);

        // XLIO Socket API.
        SET_EXTRA_API(xlio_init_ex, xlio_init_ex, XLIO_EXTRA_API_XLIO_ULTRA);
        SET_EXTRA_API(xlio_exit, xlio_exit, XLIO_EXTRA_API_XLIO_ULTRA);
        SET_EXTRA_API(xlio_poll_group_create, xlio_poll_group_create, XLIO_EXTRA_API_XLIO_ULTRA);
        SET_EXTRA_API(xlio_poll_group_destroy, xlio_poll_group_destroy, XLIO_EXTRA_API_XLIO_ULTRA);
        SET_EXTRA_API(xlio_poll_group_poll, xlio_poll_group_poll, XLIO_EXTRA_API_XLIO_ULTRA);
        SET_EXTRA_API(xlio_socket_create, xlio_socket_create, XLIO_EXTRA_API_XLIO_ULTRA);
        SET_EXTRA_API(xlio_socket_destroy, xlio_socket_destroy, XLIO_EXTRA_API_XLIO_ULTRA);
        SET_EXTRA_API(xlio_socket_update, xlio_socket_update, XLIO_EXTRA_API_XLIO_ULTRA);
        SET_EXTRA_API(xlio_socket_setsockopt, xlio_socket_setsockopt, XLIO_EXTRA_API_XLIO_ULTRA);
        SET_EXTRA_API(xlio_socket_getsockname, xlio_socket_getsockname, XLIO_EXTRA_API_XLIO_ULTRA);
        SET_EXTRA_API(xlio_socket_getpeername, xlio_socket_getpeername, XLIO_EXTRA_API_XLIO_ULTRA);
        SET_EXTRA_API(xlio_socket_bind, xlio_socket_bind, XLIO_EXTRA_API_XLIO_ULTRA);
        SET_EXTRA_API(xlio_socket_connect, xlio_socket_connect, XLIO_EXTRA_API_XLIO_ULTRA);
        SET_EXTRA_API(xlio_socket_listen, xlio_socket_listen, XLIO_EXTRA_API_XLIO_ULTRA);
        SET_EXTRA_API(xlio_socket_get_pd, xlio_socket_get_pd, XLIO_EXTRA_API_XLIO_ULTRA);
        SET_EXTRA_API(xlio_socket_detach_group, xlio_socket_detach_group,
                      XLIO_EXTRA_API_XLIO_ULTRA);
        SET_EXTRA_API(xlio_socket_attach_group, xlio_socket_attach_group,
                      XLIO_EXTRA_API_XLIO_ULTRA);
        SET_EXTRA_API(xlio_socket_send, xlio_socket_send, XLIO_EXTRA_API_XLIO_ULTRA);
        SET_EXTRA_API(xlio_socket_sendv, xlio_socket_sendv, XLIO_EXTRA_API_XLIO_ULTRA);
        SET_EXTRA_API(xlio_poll_group_flush, xlio_poll_group_flush, XLIO_EXTRA_API_XLIO_ULTRA);
        SET_EXTRA_API(xlio_socket_flush, xlio_socket_flush, XLIO_EXTRA_API_XLIO_ULTRA);
        SET_EXTRA_API(xlio_socket_buf_free, xlio_socket_buf_free, XLIO_EXTRA_API_XLIO_ULTRA);
        SET_EXTRA_API(xlio_poll_group_buf_free, xlio_poll_group_buf_free,
                      XLIO_EXTRA_API_XLIO_ULTRA);
        SET_EXTRA_API(xlio_socket_buf_get_mkey, xlio_socket_buf_get_mkey,
                      XLIO_EXTRA_API_XLIO_ULTRA);
        SET_EXTRA_API(xlio_socket_buf_get_pd, xlio_socket_buf_get_pd, XLIO_EXTRA_API_XLIO_ULTRA);
        SET_EXTRA_API(xlio_socket_buf_get_data, xlio_socket_buf_get_data,
                      XLIO_EXTRA_API_XLIO_ULTRA);
        SET_EXTRA_API(xlio_socket_buf_get_size, xlio_socket_buf_get_size,
                      XLIO_EXTRA_API_XLIO_ULTRA);

        /* Zero-copy receive for BSD sockets with kTLS/UTLS-RX. */
        SET_EXTRA_API(xlio_recv_zc_fd, xlio_recv_zc_fd, XLIO_EXTRA_API_RECV_ZC);
        SET_EXTRA_API(xlio_recv_zc_release, xlio_recv_zc_release, XLIO_EXTRA_API_RECV_ZC);

        /* Convert existing fd to xlio_socket_t without disrupting the connection. */
        SET_EXTRA_API(xlio_socket_from_fd, xlio_socket_from_fd, XLIO_EXTRA_API_SOCKET_FROM_FD);
    }

    return &xlio_api;
}

/*
 * XLIO Ultra API
 */

extern "C" int xlio_init_ex(const struct xlio_init_attr *attr)
{
    if (g_init_global_ctors_done) {
        vlog_printf(VLOG_DEBUG, "XLIO is already initialized!!\n");
        // If XLIO global memory is already allocated, we can't
        // reinitialize XLIO with new memory allocator
        if (attr->memory_alloc) {
            errno = EEXIST;
            return -1;
        } else {
            // XLIO is already initialized, no need to initialize again.
            return 0;
        }
    }

    xlio_init();

    extern xlio_memory_cb_t g_user_memory_cb;
    g_user_memory_cb = attr->memory_cb;

    if (attr->memory_alloc) {
        mce_sys_var::safe_instance().user_alloc.memalloc = attr->memory_alloc;
        mce_sys_var::safe_instance().user_alloc.memfree = attr->memory_free;
        mce_sys_var::safe_instance().memory_limit_user =
            std::max(safe_mce_sys().memory_limit_user, safe_mce_sys().memory_limit);
    }

    DO_GLOBAL_CTORS();

    return 0;
}

extern "C" int xlio_poll_group_create(const struct xlio_poll_group_attr *attr,
                                      xlio_poll_group_t *group_out)
{
    // Validate input arguments
    if (!group_out || !attr || !attr->socket_event_cb) {
        errno = EINVAL;
        return -1;
    }

    poll_group *grp = new poll_group(*attr);
    if (!grp) {
        errno = ENOMEM;
        return -1;
    }

    *group_out = reinterpret_cast<xlio_poll_group_t>(grp);
    return 0;
}

extern "C" int xlio_poll_group_destroy(xlio_poll_group_t group)
{
    poll_group *grp = reinterpret_cast<poll_group *>(group);

    delete grp;
    return 0;
}

extern "C" int xlio_poll_group_update(xlio_poll_group_t group,
                                      const struct xlio_poll_group_attr *attr)
{
    poll_group *grp = reinterpret_cast<poll_group *>(group);

    if (!attr || !attr->socket_event_cb) {
        errno = EINVAL;
        return -1;
    }
    return grp->update(attr);
}

extern "C" void xlio_poll_group_poll(xlio_poll_group_t group)
{
    poll_group *grp = reinterpret_cast<poll_group *>(group);

    grp->poll();
}

extern "C" int xlio_socket_create(const struct xlio_socket_attr *attr, xlio_socket_t *sock_out)
{
    // Validate input arguments
    if (!sock_out || !attr || !attr->group ||
        !(attr->domain == AF_INET || attr->domain == AF_INET6)) {
        errno = EINVAL;
        return -1;
    }

    int fd = SYSCALL(socket, attr->domain, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    sockinfo_tcp *si = new sockinfo_tcp(fd, attr->domain);
    if (!si) {
        errno = ENOMEM;
        return -1;
    }
    si->set_xlio_socket(attr);
    // App just created this socket so surely it is aware of it.
    si->set_xlio_app_callbacks_allowed();

    poll_group *grp = reinterpret_cast<poll_group *>(attr->group);
    grp->add_socket(si);

    *sock_out = reinterpret_cast<xlio_socket_t>(si);
    return 0;
}

extern "C" int xlio_socket_destroy(xlio_socket_t sock)
{
    sockinfo_tcp *si = reinterpret_cast<sockinfo_tcp *>(sock);
    poll_group *grp = si->get_poll_group();

    if (unlikely(!si->is_xlio_socket())) {
        errno = EINVAL;
        return -1;
    }

    if (likely(grp)) {
        grp->mark_socket_to_close(si);
    } else {
        // Detached socket flow.
        g_p_fd_collection->clear_socket(si->get_fd());
        si->prepare_to_close(true);
        si->clean_socket_obj();
    }
    return 0;
}

extern "C" int xlio_socket_update(xlio_socket_t sock, unsigned flags, uintptr_t userdata_sq)
{
    sockinfo_tcp *si = reinterpret_cast<sockinfo_tcp *>(sock);
    return si->update_xlio_socket(flags, userdata_sq);
}

extern "C" int xlio_socket_setsockopt(xlio_socket_t sock, int level, int optname,
                                      const void *optval, socklen_t optlen)
{
    sockinfo_tcp *si = reinterpret_cast<sockinfo_tcp *>(sock);
    int errno_save = errno;

    int rc = si->setsockopt(level, optname, optval, optlen);
    if (rc == 0) {
        errno = errno_save;
    }
    return rc;
}

extern "C" int xlio_socket_getsockname(xlio_socket_t sock, struct sockaddr *addr,
                                       socklen_t *addrlen)
{
    sockinfo_tcp *si = reinterpret_cast<sockinfo_tcp *>(sock);
    return si->getsockname(addr, addrlen);
}

extern "C" int xlio_socket_getpeername(xlio_socket_t sock, struct sockaddr *addr,
                                       socklen_t *addrlen)
{
    sockinfo_tcp *si = reinterpret_cast<sockinfo_tcp *>(sock);
    return si->getpeername(addr, addrlen);
}

extern "C" int xlio_socket_bind(xlio_socket_t sock, const struct sockaddr *addr, socklen_t addrlen)
{
    sockinfo_tcp *si = reinterpret_cast<sockinfo_tcp *>(sock);
    int errno_save = errno;

    int rc = si->bind(addr, addrlen);
    if (rc == 0) {
        errno = errno_save;
        if (si->isPassthrough()) {
            rc = -1;
            errno = ENODEV;
        }
    }
    return rc;
}

extern "C" int xlio_socket_connect(xlio_socket_t sock, const struct sockaddr *to, socklen_t tolen)
{
    sockinfo_tcp *si = reinterpret_cast<sockinfo_tcp *>(sock);
    int errno_save = errno;
    int rc = 0;

    if (!si->isPassthrough()) {
        rc = si->connect(to, tolen);
    }
    if (si->isPassthrough()) {
        rc = -1;
        errno = ENODEV;
    }
    rc = (rc == -1 && (errno == EINPROGRESS || errno == EAGAIN)) ? 0 : rc;
    if (rc == 0) {
        errno = errno_save;
    }
    return rc;
}

extern "C" int xlio_socket_listen(xlio_socket_t sock)
{
    sockinfo_tcp *si = reinterpret_cast<sockinfo_tcp *>(sock);
    poll_group *group = si->get_poll_group();

    if (!group->m_socket_accept_cb) {
        errno = ENOTCONN;
        return -1;
    }

    int rc = si->prepareListen();
    if (rc == 1) {
        errno = ENODEV;
        rc = -1;
    }
    return rc ?: si->listen(-1);
}

extern "C" struct ibv_pd *xlio_socket_get_pd(xlio_socket_t sock)
{
    sockinfo_tcp *si = reinterpret_cast<sockinfo_tcp *>(sock);
    ib_ctx_handler *ctx = si->get_ctx();

    return ctx ? ctx->get_ibv_pd() : nullptr;
}

int xlio_socket_detach_group(xlio_socket_t sock)
{
    sockinfo_tcp *si = reinterpret_cast<sockinfo_tcp *>(sock);

    return si->detach_xlio_group();
}

int xlio_socket_attach_group(xlio_socket_t sock, xlio_poll_group_t group)
{
    sockinfo_tcp *si = reinterpret_cast<sockinfo_tcp *>(sock);
    poll_group *grp = reinterpret_cast<poll_group *>(group);

    return si->attach_xlio_group(grp);
}

static void xlio_buf_free(struct xlio_buf *buf)
{
    mem_buf_desc_t *desc = mem_buf_desc_t::from_xlio_buf(buf);
    ring_slave *rng = desc->p_desc_owner;

    desc->p_next_desc = nullptr;
    bool ret = rng->reclaim_recv_buffers(desc);
    if (unlikely(!ret)) {
        g_buffer_pool_rx_ptr->put_buffer_after_deref_thread_safe(desc);
    }
}

/*
 * ---------------------------------------------------------------------------
 * Zero-copy receive for BSD sockets with kTLS / UTLS-RX
 * ---------------------------------------------------------------------------
 *
 * xlio_recv_zc_fd() resolves the file descriptor to its sockinfo_tcp and
 * delegates to sockinfo_tcp::recv_zc_impl().  This keeps the hot-path logic
 * inside the sockinfo where all the state lives.
 *
 * xlio_recv_zc_release() is intentionally simple: it follows the same path
 * as xlio_buf_free() used by the Ultra API.  No fd needed because the
 * mem_buf_desc_t already carries p_desc_owner.
 */

extern "C" EXPORT_SYMBOL int xlio_recv_zc_fd(int fd, struct xlio_zc_seg *segs, int max_segs)
{
    if (!g_p_fd_collection) {
        errno = ENOTSUP;
        return -1;
    }

    /*
     * Look up the sockinfo for this fd.  get_sockfd() is a plain array
     * lookup with no refcount overhead — the socket lifetime is pinned by
     * the application's file descriptor; it cannot be destroyed while the
     * fd is open.
     */
    sockinfo *si_base = g_p_fd_collection->get_sockfd(fd);
    if (!si_base) {
        errno = ENOTSUP;
        return -1;
    }

    /* We only support TCP sockets. */
    sockinfo_tcp *si = dynamic_cast<sockinfo_tcp *>(si_base);
    if (!si) {
        errno = ENOTSUP;
        return -1;
    }

    return si->recv_zc_impl(segs, max_segs);
}

extern "C" EXPORT_SYMBOL void xlio_recv_zc_release(struct xlio_buf *buf)
{
    if (buf) {
        mem_buf_desc_t *desc = mem_buf_desc_t::from_xlio_buf(buf);
        (void)desc;
        xlio_buf_free(buf);
    }
}

/*
 * xlio_socket_from_fd — obtain an xlio_socket_t handle for an existing fd.
 *
 * This is a lightweight read-only conversion: the fd continues to own the
 * underlying connection.  The caller must NOT pass the returned handle to
 * xlio_socket_destroy(); closing the original fd tears down the connection.
 *
 * The function does not install the Ultra API rx/lwip callbacks, so
 * xlio_socket_rx_cb_t will not fire.  The TX path (xlio_socket_sendv) works
 * for cleartext connections.  For connections with UTLS_TX, xlio_socket_sendv
 * bypasses sockinfo_tcp_ops_tls::tcp_tx and does NOT add TLS record headers;
 * use SSL_write for TLS-encrypted sends until a TLS-aware express-send path
 * is added.
 */
extern "C" EXPORT_SYMBOL xlio_socket_t xlio_socket_from_fd(int fd)
{
    sockinfo *base = g_p_fd_collection ? g_p_fd_collection->get_sockfd(fd) : nullptr;
    if (!base) {
        errno = ENOTSUP;
        return 0;
    }
    sockinfo_tcp *si = dynamic_cast<sockinfo_tcp *>(base);
    if (!si) {
        errno = ENOTSUP;
        return 0;
    }
    return reinterpret_cast<xlio_socket_t>(si);
}

extern "C" void xlio_socket_buf_free(xlio_socket_t sock, struct xlio_buf *buf)
{
    NOT_IN_USE(sock);
    xlio_buf_free(buf);
}

extern "C" void xlio_poll_group_buf_free(xlio_poll_group_t group, struct xlio_buf *buf)
{
    NOT_IN_USE(group);
    xlio_buf_free(buf);
}

extern "C" uint32_t xlio_socket_buf_get_mkey(struct xlio_buf *buf)
{
    if (!buf) {
        return 0;
    }

    mem_buf_desc_t *desc = mem_buf_desc_t::from_xlio_buf(buf);
    if (!desc) {
        return 0;
    }

    /* ZERO-COPY FIX: Handle STRQ stride buffers
     * Stride buffers don't have their own lkey - they're views into a parent buffer */
    if (desc->lwip_pbuf.desc.attr == PBUF_DESC_STRIDE && desc->lwip_pbuf.desc.mdesc) {
        /* Get lkey from parent buffer */
        mem_buf_desc_t *parent = reinterpret_cast<mem_buf_desc_t *>(desc->lwip_pbuf.desc.mdesc);
        return parent->lkey;
    }

    return desc->lkey;
}

extern "C" struct ibv_pd *xlio_socket_buf_get_pd(struct xlio_buf *buf)
{
    if (!buf) {
        return nullptr;
    }

    mem_buf_desc_t *desc = mem_buf_desc_t::from_xlio_buf(buf);
    if (!desc) {
        return nullptr;
    }

    /* ZERO-COPY FIX: Handle STRQ stride buffers
     * Get owner from parent buffer if this is a stride */
    ring_slave *owner = desc->p_desc_owner;
    if (!owner && desc->lwip_pbuf.desc.attr == PBUF_DESC_STRIDE && desc->lwip_pbuf.desc.mdesc) {
        mem_buf_desc_t *parent = reinterpret_cast<mem_buf_desc_t *>(desc->lwip_pbuf.desc.mdesc);
        owner = parent->p_desc_owner;
    }

    if (!owner) {
        return nullptr;
    }

    ib_ctx_handler *ctx = owner->get_ctx(0);
    return ctx ? ctx->get_ibv_pd() : nullptr;
}

extern "C" void *xlio_socket_buf_get_data(struct xlio_buf *buf)
{
    if (!buf) {
        return nullptr;
    }

    mem_buf_desc_t *desc = mem_buf_desc_t::from_xlio_buf(buf);
    return desc ? desc->p_buffer : nullptr;
}

extern "C" size_t xlio_socket_buf_get_size(struct xlio_buf *buf)
{
    if (!buf) {
        return 0;
    }

    mem_buf_desc_t *desc = mem_buf_desc_t::from_xlio_buf(buf);
    return desc ? desc->sz_buffer : 0;
}

extern "C" int xlio_socket_send(xlio_socket_t sock, const void *data, size_t len,
                                const struct xlio_socket_send_attr *attr)
{
    const struct iovec iov = {.iov_base = const_cast<void *>(data), .iov_len = len};

    return xlio_socket_sendv(sock, &iov, 1, attr);
}

/*
 * [TEST-ZC] ZcRxOwner — thin mem_desc wrapper around an XLIO RX buffer.
 *
 * Passed as zc_owner to sockinfo_tcp_ops_tls::tx() so that tls_record uses
 * the zero-copy append_data path (pointer store, no memcpy) and fill_iov
 * produces a 3-element scatter-gather: [TLS header | RX DMA payload | trailer].
 * The NIC reads directly from the RX DMA address and encrypts in hardware.
 *
 * Lifecycle:
 *   ref_ starts at 1 (caller holds).
 *   tls_record constructor calls get() → ref_=2.
 *   Caller calls put() after si->tx() → ref_=1 (tls_record owns).
 *   TCP ACK → ~tls_record() calls put() → ref_=0 → xlio_buf_free + delete.
 *
 * Remove before merging.
 */
#ifdef DEFINED_UTLS
class ZcRxOwner final : public mem_desc {
public:
    explicit ZcRxOwner(struct xlio_buf *buf) : buf_(buf), ref_(1) {}

    void get() override { ref_.fetch_add(1, std::memory_order_relaxed); }

    void put() override
    {
        if (ref_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            /* [TEST-ZC] TCP ACK confirmed — all NIC DMA reads complete.
             * In production this is where we call the completion callback
             * (e.g. downstream->resume_read() or xlio_socket_comp_cb_t). */
            // fprintf(stderr,
            //         "[TEST-ZC] ZcRxOwner::put ref=0 buf=%p → xlio_buf_free sz=%zu "
            //         "(TCP ACK confirmed, buffer safe to reuse)\n",
            //         static_cast<void *>(buf_), xlio_socket_buf_get_size(buf_));
            xlio_buf_free(buf_);
            delete this;
        }
    }

    uint32_t get_lkey(mem_buf_desc_t *, ib_ctx_handler *, const void *,
                      size_t) override
    {
        return xlio_socket_buf_get_mkey(buf_);
    }

private:
    struct xlio_buf *buf_;
    std::atomic<int> ref_;
};
#endif /* DEFINED_UTLS */

/*
 * xlio_buf_addref — increment the lwip pbuf reference count on an RX DMA
 * buffer obtained from xlio_recv_zc_fd().
 *
 * Call once for each additional zero-copy TX send that will reference this
 * buffer beyond the first (the first send consumes the ref given by
 * xlio_recv_zc_fd()).  Each ref is released by xlio_buf_free() when the
 * corresponding TCP ACK fires in ZcRxOwner::put().
 *
 * This is an EXPORT_SYMBOL so that nghttp2 (running against the shared
 * library) can resolve it via dlsym(RTLD_DEFAULT, "xlio_buf_addref").
 */
extern "C" __attribute__((visibility("default")))
void xlio_buf_addref(struct xlio_buf *buf)
{
    if (!buf) return;
    reinterpret_cast<mem_buf_desc_t *>(buf)->lwip_pbuf_inc_ref_count();
}

extern "C" int xlio_socket_sendv(xlio_socket_t sock, const struct iovec *iov, unsigned iovcnt,
                                 const struct xlio_socket_send_attr *attr)
{
    sockinfo_tcp *si = reinterpret_cast<sockinfo_tcp *>(sock);

#ifdef DEFINED_UTLS
    /*
     * TLS-aware express send.
     *
     * tcp_tx_express{_inline} bypass m_ops->tx(), so for a UTLS_TX socket
     * they produce raw TCP payload with no TLS record framing.  Redirect
     * through si->tx() → sockinfo_tcp_ops_tls::tx() instead.
     *
     * Level 1 (inline, attr->userdata_op == 0):
     *   rec->append_data() copies plaintext into the TLS record buffer.
     *   NIC encrypts during DMA TX.
     *
     * Level 2 ([TEST-ZC], attr->userdata_op != 0):
     *   attr->userdata_op carries the xlio_buf* from xlio_recv_zc_fd().
     *   A ZcRxOwner is created and passed as zc_owner to tls_record.
     *   rec->append_data() stores the pointer (no copy).
     *   fill_iov() produces [TLS header | RX DMA ptr | trailer].
     *   NIC reads from the original RX DMA buffer and encrypts in hardware.
     *   Caller must NOT call xlio_recv_zc_release() for this buf;
     *   ZcRxOwner::put() releases it on TCP ACK.
     */
    if (dynamic_cast<sockinfo_tcp_ops_tls *>(si->get_ops())) {
        static std::atomic<bool> s_first{true};
        // if (s_first.exchange(false, std::memory_order_relaxed)) {
        //     fprintf(stderr,
        //             "[xlio-ultra] xlio_socket_sendv: UTLS_TX active → routing through "
        //             "TLS ops\n");
        // }
        xlio_tx_call_attr_t tx_arg;
        tx_arg.opcode      = TX_WRITE;
        tx_arg.attr.iov    = const_cast<struct iovec *>(iov);
        tx_arg.attr.sz_iov = static_cast<ssize_t>(iovcnt);
        tx_arg.attr.flags  = (attr->flags & XLIO_SOCKET_SEND_FLAG_FLUSH) ? 0 : MSG_MORE;

        /* [TEST-ZC] Level-2 zero-copy: userdata_op carries the xlio_buf *. */
        ZcRxOwner *zc = nullptr;
        if (attr->userdata_op != 0) {
            struct xlio_buf *rxbuf =
                reinterpret_cast<struct xlio_buf *>(attr->userdata_op);
            zc = new (std::nothrow) ZcRxOwner(rxbuf);
            if (zc) {
                tx_arg.attr.flags |= MSG_ZEROCOPY;
                tx_arg.priv.mdesc  = reinterpret_cast<void *>(zc);
            }
        }

        ssize_t rc = si->tx(tx_arg);
        /* Release caller's initial ref; tls_record holds its own via get(). */
        if (zc) zc->put();
        return static_cast<int>(rc);
    }
#endif /* DEFINED_UTLS */

    /* Cleartext / non-TLS path — original express logic. */
    unsigned flags = XLIO_EXPRESS_OP_TYPE_DESC;
    flags |= !(attr->flags & XLIO_SOCKET_SEND_FLAG_FLUSH) * XLIO_EXPRESS_MSG_MORE;

    int rc = (attr->flags & XLIO_SOCKET_SEND_FLAG_INLINE)
        ? si->tcp_tx_express_inline(iov, iovcnt, flags)
        : si->tcp_tx_express(iov, iovcnt, attr->mkey, flags,
                             reinterpret_cast<void *>(attr->userdata_op));
    return rc < 0 ? rc : 0;
}

extern "C" void xlio_poll_group_flush(xlio_poll_group_t group)
{
    poll_group *grp = reinterpret_cast<poll_group *>(group);
    grp->flush();
}

extern "C" void xlio_socket_flush(xlio_socket_t sock)
{
    sockinfo_tcp *si = reinterpret_cast<sockinfo_tcp *>(sock);
    si->flush();
}
