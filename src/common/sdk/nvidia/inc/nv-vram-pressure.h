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
 * Diagnostic counters for video memory pressure, shown in
 * /proc/driver/nvidia/vram_pressure. Each event counts how often a path
 * that only runs when memory is short was taken, the bytes involved and
 * when it last happened (CLOCK_MONOTONIC), so a sampler can tell when a
 * workload ran out of video memory without tracing every RM call.
 *
 * This header has two identical copies: src/common/sdk/nvidia/inc for RM
 * and kernel-open/common/inc for the kernel modules.
 */

#ifndef _NV_VRAM_PRESSURE_H_
#define _NV_VRAM_PRESSURE_H_

#include <nvtypes.h>
#include <nv-kernel-interface-api.h>

typedef enum
{
    /* RM: a video memory allocation failed for lack of memory. */
    NV_VRAM_PRESSURE_VIDMEM_ALLOC_FAIL = 0,
    /* RM: a system memory allocation was retried with smaller pages. */
    NV_VRAM_PRESSURE_SYSMEM_PAGE_RETRY,
    /* RM: NV_ESC_RM_MAP_MEMORY_DMA returned an error. */
    NV_VRAM_PRESSURE_DMA_MAP_FAIL,
    /* RM: NV_ESC_RM_UNMAP_MEMORY_DMA returned an error. */
    NV_VRAM_PRESSURE_DMA_UNMAP_FAIL,
    /* nvidia-drm: a compressible GEM allocation was retried without compression. */
    NV_VRAM_PRESSURE_GEM_NO_COMPRESSION,
    /* nvidia-drm: an offscreen GEM allocation fell back to system memory. */
    NV_VRAM_PRESSURE_GEM_SYSMEM_FALLBACK,
    /* nvidia-drm: a GEM allocation failed. */
    NV_VRAM_PRESSURE_GEM_ALLOC_FAIL,
    /* nvidia-drm: a GEM allocation was refused by the allocation back-off. */
    NV_VRAM_PRESSURE_GEM_BACKOFF,
    NV_VRAM_PRESSURE_EVENT_COUNT
} nv_vram_pressure_event_t;

/* Count one event of the given kind involving the given number of bytes. */
void NV_API_CALL os_vram_pressure_event(NvU32 event, NvU64 bytes);

/* Read one counter: events, bytes and the time of the last event in ns. */
void NV_API_CALL os_vram_pressure_get(NvU32 event, NvU64 *count, NvU64 *bytes,
                                      NvU64 *last_ns);

#endif /* _NV_VRAM_PRESSURE_H_ */
