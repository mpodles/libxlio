/*
 * SPDX-FileCopyrightText: NVIDIA CORPORATION & AFFILIATES
 * Copyright (c) 2021-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: GPL-2.0-only or BSD-2-Clause
 */

#ifndef _SOCK_EXTRA_H_
#define _SOCK_EXTRA_H_

#include "xlio_extra.h"
#include "utils/compiler.h"

struct xlio_api_t *extra_api();

#ifdef __cplusplus
extern "C" {
#endif

EXPORT_SYMBOL int  xlio_recv_zc_fd(int fd, struct xlio_zc_seg *segs, int max_segs);
EXPORT_SYMBOL void xlio_recv_zc_release(struct xlio_buf *buf);

#ifdef __cplusplus
}
#endif

#endif /* _SOCK_EXTRA_H_ */
