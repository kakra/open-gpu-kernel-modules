/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Kai Krakow
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/*
 * Diagnostic counters for fixes of the ogkm series, shown in
 * /proc/driver/nvidia/fix_hits. Each event counts how often a fix took the
 * path that the unpatched driver got wrong, the bytes involved (0 where
 * there are none) and when it last happened (CLOCK_MONOTONIC), so it can be
 * told which fixes still do anything on a given workload.
 *
 * This header has two identical copies: src/common/sdk/nvidia/inc for RM
 * and kernel-open/common/inc for the kernel modules.
 */

#ifndef _NV_FIX_HITS_H_
#define _NV_FIX_HITS_H_

#include <nvtypes.h>
#include <nv-kernel-interface-api.h>

typedef enum
{
    /* RM: an unmap starting in a released page unmapped the client remainder. */
    NV_FIX_HIT_DMA_UNMAP_HOLE_REMAINDER = 0,
    /* RM: an unmap skipped another resource's DMA mapping found by VA. */
    NV_FIX_HIT_DMA_UNMAP_FOREIGN_SKIPPED,
    /* RM: a destroyed VirtualMemory still had a DMA mapping, which was freed. */
    NV_FIX_HIT_DMA_LEFTOVER_FREED,
    /* RM: a failed semsurf waiter registration kept another waiter's state. */
    NV_FIX_HIT_SEMSURF_WAITER_KEPT,
    /* RM: the OOB Xid RPC was skipped in atomic context or in an RPC poll. */
    NV_FIX_HIT_OOB_XID_RPC_SKIPPED,
    /* nvidia: the ISR bottom half ran for an inaccessible GPU to release the lock. */
    NV_FIX_HIT_ISR_BH_INACCESSIBLE,
    /* nvidia: a GPU survived a failed non-fatal AER recovery and was kept. */
    NV_FIX_HIT_AER_RECOVERY_SURVIVED,
    /* nvidia: a hwmon read hit a GSP write of the sensor page and was retried. */
    NV_FIX_HIT_HWMON_READ_RETRY,
    NV_FIX_HIT_EVENT_COUNT
} nv_fix_hit_event_t;

/* Count one hit of the given fix involving the given number of bytes. */
void NV_API_CALL os_fix_hit_event(NvU32 event, NvU64 bytes);

/* Read one counter: hits, bytes and the time of the last hit in ns. */
void NV_API_CALL os_fix_hit_get(NvU32 event, NvU64 *count, NvU64 *bytes,
                                NvU64 *last_ns);

#endif /* _NV_FIX_HITS_H_ */
