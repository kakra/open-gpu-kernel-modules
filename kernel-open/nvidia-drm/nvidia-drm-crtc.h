/*
 * Copyright (c) 2016-2026, NVIDIA CORPORATION. All rights reserved.
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
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#ifndef __NVIDIA_DRM_CRTC_H__
#define __NVIDIA_DRM_CRTC_H__

#include "nvidia-drm-conftest.h"

#if defined(NV_DRM_AVAILABLE)

#include "nvidia-drm-helper.h"

#if defined(NV_DRM_HAS_COLOROP)
#include "nvidia-drm-color-pipeline.h"
#endif

#if defined(NV_DRM_DRMP_H_PRESENT)
#include <drm/drmP.h>
#endif

#include <drm/drm_crtc.h>

#include "nvtypes.h"
#include "nvkms-kapi.h"
#include "nv_drm_common_ioctl.h"

#define NV_DRM_VBLANK_HISTORY 16

struct nv_drm_crtc {
    NvU32 head;

#if defined(NV_DRM_CRTC_FUNCS_HAS_GET_VBLANK_TIMESTAMP)
    /* Last vblank timestamp stored when callback occurs (nanoseconds) */
    NvU64 last_vblank_time;

    /*
     * Recently handled vblanks (hardware timestamp and DRM vblank count),
     * oldest first starting at vblank_history_next - vblank_history_len.
     * Used to report flip events at the vblank the flip happened at, see
     * nv_drm_crtc_handle_flip_events(). Protected by dev->vblank_time_lock.
     */
    struct {
        NvU64 time;
        u64 count;
    } vblank_history[NV_DRM_VBLANK_HISTORY];
    unsigned int vblank_history_len;
    unsigned int vblank_history_next;

    /*
     * Length of the vertical blanking interval of the current mode, so the
     * start of a vblank can be derived from its timestamp (which points at
     * the first active line). Protected by dev->vblank_time_lock.
     */
    NvU64 vblank_duration_ns;

    /*
     * Length of a whole frame of the current mode, zero if unknown.
     * Protected by dev->vblank_time_lock.
     */
    NvU64 vblank_period_ns;

    /*
     * With VRR the raster waits at the end of the front porch until a flip
     * arrives, see nv_drm_crtc_handle_flip_events(). These are the length of
     * the active scanout, and the time from the end of that wait to the first
     * active line. Protected by dev->vblank_time_lock.
     */
    NvU64 vblank_active_ns;
    NvU64 vblank_restart_to_active_ns;

    /*
     * First active line of the last flip whose event was sent, and with it
     * when its scanout ends. Protected by dev->event_lock.
     */
    NvU64 last_flip_first_active;

    /*
     * Flips whose completion event arrived before the vblank they happened
     * at was handled; their DRM events are sent from the vblank callback.
     * Protected by dev->event_lock.
     */
    struct list_head pending_flip_events;

    /* Sends pending flip events if no vblank callback arrives in time */
    struct delayed_work pending_flip_events_work;
#endif

    /* True if a real vblank interrupt has fired since vblank was enabled.
     *
     * It is protected by dev->vblank_time_lock that is DRM core's designated
     * spinlock for protecting vblank timestamps. The flag's validity is
     * dependent on the timestamp data itself, protecting both with the same
     * lock prevents race where the flag might indicate "valid" while the
     * timestamp is stale.
     */
    bool vblank_intr_fired;

    /*
     * NVKMS reported VRR active for a flip on this CRTC, and the
     * VRR_ENABLED property has stayed set since. Only used from commits,
     * which the CRTC's modeset lock serializes.
     */
    bool vrr_active;

    /**
     * @flip_list:
     *
     * List of flips pending to get processed by __nv_drm_handle_flip_event().
     * Protected by @flip_list_lock.
     */
    struct list_head flip_list;

    struct mutex vblank_q_lock;

    /**
     * @flip_list_lock:
     *
     * Spinlock to protect @flip_list.
     */
    spinlock_t flip_list_lock;

    /**
     * @flips_enqueued, @flips_completed:
     *
     * Number of flips added to and removed from @flip_list, i.e. submitted
     * with a hardware flip event and completed. Protected by
     * @flip_list_lock.
     */
    u64 flips_enqueued;
    u64 flips_completed;

    /**
     * @modeset_permission_filep:
     *
     * The filep using this crtc with DRM_IOCTL_NVIDIA_GRANT_PERMISSIONS.
     */
    struct drm_file *modeset_permission_filep;

    struct NvKmsKapiVblankIntrCallback *vblankIntrCallback;

    nv_drm_work disable_vblank_work;
    nv_drm_work enable_vblank_work;

    struct NvKmsLUTCaps olut_caps;

    struct drm_crtc base;
};

/**
 * struct nv_drm_flip - flip state
 *
 * This state is getting used to consume DRM completion event associated
 * with each crtc state from atomic commit.
 *
 * Function nv_drm_atomic_apply_modeset_config() consumes DRM completion
 * event, save it into flip state associated with crtc and queue flip state into
 * crtc's flip list and commits atomic update to hardware.
 */
struct nv_drm_flip {
    /**
     * @event:
     *
     * Optional pointer to a DRM event to signal upon completion of
     * the state update.
     */
    struct drm_pending_vblank_event *event;

    /**
     * @pending_events
     *
     * Number of HW events pending to signal completion of the state
     * update.
     */
    uint32_t pending_events;

    /**
     * @list_entry:
     *
     * Entry on the per-CRTC &nv_drm_crtc.flip_list. Protected by
     * &nv_drm_crtc.flip_list_lock.
     */
    struct list_head list_entry;

    /**
     * @deferred_flip_list
     *
     * List flip objects whose processing is deferred until processing of
     * this flip object. Protected by &nv_drm_crtc.flip_list_lock.
     * nv_drm_atomic_commit() gets last flip object from
     * nv_drm_crtc:flip_list and add deferred flip objects into
     * @deferred_flip_list, __nv_drm_handle_flip_event() processes
     * @deferred_flip_list.
     */
    struct list_head deferred_flip_list;

    /**
     * @submit_time:
     *
     * CLOCK_MONOTONIC time (ns) just before the flip was handed to NVKMS. The
     * flip happens at the first vblank starting after NVKMS kicked it off to
     * the hardware, which is after this. Zero if the flip may happen later
     * (tearing flips, unsignaled acquire fences).
     */
    NvU64 submit_time;

    /**
     * @kick_time:
     *
     * CLOCK_MONOTONIC time (ns) just after NVKMS returned, so the hardware
     * got the flip before this. Zero until then. Only set while the flip is
     * in &nv_drm_crtc.flip_list, see nv_drm_crtc_set_flip_kick_time().
     */
    NvU64 kick_time;

    /**
     * @event_time:
     *
     * CLOCK_MONOTONIC time (ns) the flip-occurred event was handled, so the
     * flip happened before this. Protected by &drm_device.event_lock.
     */
    NvU64 event_time;

    /*
     * @vrr:
     *
     * The raster may be waiting for this flip, so the vblank timestamps
     * don't tell when it is scanned out. See
     * nv_drm_crtc_handle_flip_events().
     */
    bool vrr;
};

struct nv_drm_crtc_state {
    /**
     * @base:
     *
     * Base DRM crtc state object for this.
     */
    struct drm_crtc_state base;

    /**
     * @head_req_config:
     *
     * Requested head's modeset configuration corresponding to this crtc state.
     */
    struct NvKmsKapiHeadRequestedConfig req_config;

    /**
     * @nv_flip:
     *
     * Flip state associated with this crtc state, gets allocated
     * by nv_drm_atomic_crtc_duplicate_state(), on successful commit it gets
     * consumed and queued into flip list by
     * nv_drm_atomic_apply_modeset_config() and finally gets destroyed
     * by __nv_drm_handle_flip_event() after getting processed.
     *
     * In case of failure of atomic commit, this flip state getting destroyed by
     * nv_drm_atomic_crtc_destroy_state().
     */
    struct nv_drm_flip *nv_flip;

    enum nv_drm_transfer_function regamma_tf;
    struct drm_property_blob *regamma_lut;
    uint64_t regamma_divisor;
    struct nv_drm_lut_surface *regamma_drm_lut_surface;
    NvBool regamma_changed;

    NvBool postcomp_color_passthrough;
    NvBool postcomp_color_passthrough_changed;
};

static inline struct nv_drm_crtc_state *to_nv_crtc_state(struct drm_crtc_state *state)
{
    return container_of(state, struct nv_drm_crtc_state, base);
}

static inline const struct nv_drm_crtc_state *to_nv_crtc_state_const(const struct drm_crtc_state *state)
{
    return container_of(state, struct nv_drm_crtc_state, base);
}

struct nv_drm_plane {
    /**
     * @base:
     *
     * Base DRM plane object for this plane.
     */
    struct drm_plane base;

    /**
     * @defaultCompositionMode:
     *
     * Default composition blending mode of this plane.
     */
    enum NvKmsCompositionBlendingMode defaultCompositionMode;

    /**
     * @layer_idx
     *
     * Index of this plane in the per head array of layers.
     */
    uint32_t layer_idx;

    struct NvKmsLUTCaps ilut_caps;
    struct NvKmsLUTCaps tmo_caps;

#if defined(NV_DRM_HAS_COLOROP)
    struct nv_drm_color_pipeline *color_pipelines[NV_DRM_PLANE_MAX_COLOR_PIPELINES];
    int num_color_pipelines;
#endif
};

static inline struct nv_drm_plane *to_nv_plane(struct drm_plane *plane)
{
    if (plane == NULL) {
        return NULL;
    }
    return container_of(plane, struct nv_drm_plane, base);
}

struct nv_drm_nvkms_surface {
    struct NvKmsKapiDevice *pDevice;
    struct NvKmsKapiMemory *nvkms_memory;
    struct NvKmsKapiSurface *nvkms_surface;
    void *buffer;
    struct kref refcount;
};

struct nv_drm_nvkms_surface_params {
    NvU32 width;
    NvU32 height;
    size_t surface_size;
    enum NvKmsSurfaceMemoryFormat format;
};

struct nv_drm_lut_surface {
    struct nv_drm_nvkms_surface base;
    struct {
        NvU32 vssSegmentEntries;

        NvU32 vssSegments;
        enum NvKmsLUTVssType vssType;

        NvU32 lutEntries;
        enum NvKmsLUTFormat entryFormat;

    } properties;

    struct nv_drm_device *nv_dev;
    struct list_head pool_entry;
};

void nv_free_drm_lut_surface_pool(struct nv_drm_device *nv_dev);

struct nv_drm_plane_state {
    struct drm_plane_state base;
    s32 __user *fd_user_ptr;
    enum nv_drm_input_color_space input_colorspace;
#if defined(NV_DRM_HAS_HDR_OUTPUT_METADATA)
    struct drm_property_blob *hdr_output_metadata;
#endif
    struct drm_property_blob *fmt_ctm;
    struct drm_property_blob *lms_ctm;
    struct drm_property_blob *lms_to_itp_ctm;
    struct drm_property_blob *itp_to_lms_ctm;
    struct drm_property_blob *blend_ctm;
    NvBool ctms_default_to_identity;

    enum nv_drm_transfer_function degamma_tf;
    struct drm_property_blob *degamma_lut;
    uint64_t degamma_multiplier; /* S31.32 Sign-Magnitude Format */
    struct nv_drm_lut_surface *degamma_drm_lut_surface;
    NvBool degamma_changed;

    struct drm_property_blob *tmo_lut;
    struct nv_drm_lut_surface *tmo_drm_lut_surface;
    NvBool tmo_changed;

    NvBool precomp_color_passthrough;
    NvBool precomp_color_passthrough_changed;
};

static inline struct nv_drm_plane_state *to_nv_drm_plane_state(struct drm_plane_state *state)
{
    return container_of(state, struct nv_drm_plane_state, base);
}

static inline const struct nv_drm_plane_state *to_nv_drm_plane_state_const(const struct drm_plane_state *state)
{
    return container_of(state, const struct nv_drm_plane_state, base);
}

static inline struct nv_drm_crtc *to_nv_crtc(struct drm_crtc *crtc)
{
    if (crtc == NULL) {
        return NULL;
    }
    return container_of(crtc, struct nv_drm_crtc, base);
}

bool nv_drm_crtc_handle_flip_events(struct nv_drm_crtc *nv_crtc,
                                    struct nv_drm_flip **pp_nv_flip,
                                    NvU64 event_time);

bool nv_drm_crtc_has_pending_flip_events(struct nv_drm_crtc *nv_crtc);

bool nv_drm_crtc_defer_flip_to_pending(struct nv_drm_crtc *nv_crtc,
                                       struct nv_drm_flip *nv_flip);

void nv_drm_crtc_set_vblank_duration(struct nv_drm_crtc *nv_crtc,
                                     const struct drm_crtc_state *crtc_state);

void nv_drm_crtc_vblank_teardown(struct nv_drm_crtc *nv_crtc);

/*
 * CRTCs are static objects, list does not change once after initialization and
 * before teardown of device. Initialization/teardown paths are single
 * threaded, so no locking required.
 */
static inline
struct nv_drm_crtc *nv_drm_crtc_lookup(struct nv_drm_device *nv_dev, NvU32 head)
{
    struct drm_crtc *crtc;
    nv_drm_for_each_crtc(crtc, nv_dev->dev) {
        struct nv_drm_crtc *nv_crtc = to_nv_crtc(crtc);

        if (nv_crtc->head == head)  {
            return nv_crtc;
        }
    }
    return NULL;
}

/**
 * nv_drm_crtc_enqueue_flip - Enqueue nv_drm_flip object to flip_list of crtc.
 */
static inline void nv_drm_crtc_enqueue_flip(struct nv_drm_crtc *nv_crtc,
                                            struct nv_drm_flip *nv_flip)
{
    spin_lock(&nv_crtc->flip_list_lock);
    list_add(&nv_flip->list_entry, &nv_crtc->flip_list);
    nv_crtc->flips_enqueued++;
    spin_unlock(&nv_crtc->flip_list_lock);
}

/**
 * nv_drm_crtc_dequeue_flip - Dequeue nv_drm_flip object to flip_list of crtc.
 */
static inline
struct nv_drm_flip *nv_drm_crtc_dequeue_flip(struct nv_drm_crtc *nv_crtc)
{
    struct nv_drm_flip *nv_flip = NULL;
    uint32_t pending_events = 0;

    spin_lock(&nv_crtc->flip_list_lock);
    nv_flip = list_first_entry_or_null(&nv_crtc->flip_list,
                                       struct nv_drm_flip, list_entry);
    if (likely(nv_flip != NULL)) {
        /*
         * Decrement pending_event count and dequeue flip object if
         * pending_event count becomes 0.
         */
        pending_events = --nv_flip->pending_events;
        if (!pending_events) {
            list_del(&nv_flip->list_entry);
            nv_crtc->flips_completed++;
        }
    }
    spin_unlock(&nv_crtc->flip_list_lock);

    if (WARN_ON(nv_flip == NULL) || pending_events) {
        return NULL;
    }

    return nv_flip;
}

/**
 * nv_drm_crtc_set_flip_kick_time - Set the kick time of a flip that is
 * still in flip_list of crtc.
 *
 * The flip event can be handled before NVKMS returns, and the flip is freed
 * once it is dequeued, so only touch it while it is still queued. A dequeued
 * flip doesn't need the kick time any more: its event time bounds the flip.
 */
static inline void nv_drm_crtc_set_flip_kick_time(struct nv_drm_crtc *nv_crtc,
                                                  struct nv_drm_flip *nv_flip,
                                                  NvU64 kick_time)
{
    struct nv_drm_flip *nv_queued_flip;

    spin_lock(&nv_crtc->flip_list_lock);
    list_for_each_entry(nv_queued_flip, &nv_crtc->flip_list, list_entry) {
        if (nv_queued_flip == nv_flip) {
            WRITE_ONCE(nv_flip->kick_time, kick_time);
            break;
        }
    }
    spin_unlock(&nv_crtc->flip_list_lock);
}

void nv_drm_enumerate_crtcs_and_planes(
    struct nv_drm_device *nv_dev,
    const struct NvKmsKapiDeviceResourcesInfo *pResInfo);

int nv_drm_get_crtc_crc32_ioctl(struct drm_device *dev,
                                void *data, struct drm_file *filep);

int nv_drm_get_crtc_crc32_v2_ioctl(struct drm_device *dev,
                                   void *data, struct drm_file *filep);

#endif /* NV_DRM_AVAILABLE */

#endif /* __NVIDIA_DRM_CRTC_H__ */
