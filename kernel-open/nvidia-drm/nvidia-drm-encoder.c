/*
 * Copyright (c) 2015-2026, NVIDIA CORPORATION. All rights reserved.
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

#include "nvidia-drm-conftest.h" /* NV_DRM_AVAILABLE */

#if defined(NV_DRM_AVAILABLE)

#include "nvidia-drm-priv.h"
#include "nvidia-drm-encoder.h"
#include "nvidia-drm-utils.h"
#include "nvidia-drm-connector.h"
#include "nvidia-drm-crtc.h"
#include "nvidia-drm-helper.h"

#include "nvmisc.h"

/*
 * Commit fcd70cd36b9b ("drm: Split out drm_probe_helper.h")
 * moves a number of helper function definitions from
 * drm/drm_crtc_helper.h to a new drm_probe_helper.h.
 */
#if defined(NV_DRM_DRM_PROBE_HELPER_H_PRESENT)
#include <drm/drm_probe_helper.h>
#endif
#include <drm/drm_crtc_helper.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>

static void nv_drm_encoder_destroy(struct drm_encoder *encoder)
{
    struct nv_drm_encoder *nv_encoder = to_nv_encoder(encoder);

    drm_encoder_cleanup(encoder);

    nv_drm_free(nv_encoder);
}

static const struct drm_encoder_funcs nv_encoder_funcs = {
    .destroy = nv_drm_encoder_destroy,
};

/*
 * Implement "scaling mode" and underscan with the head scaler.
 *
 * "scaling mode": show a mode smaller than the display's native resolution
 * by driving the native timings with the closest refresh rate and scaling
 * the mode's area. Displays such as laptop panels often accept no other
 * timings.
 *
 * Underscan, for TVs that crop the picture edges: shrink the image by
 * "underscan hborder" pixels on the left and on the right and "underscan
 * vborder" pixels at the top and at the bottom, keeping its aspect ratio,
 * like nouveau and radeon.
 *
 * The CRTC's mode keeps the requested size, which is what the layers are
 * positioned in; the adjusted mode gets the timings that are driven.
 */
static int nv_drm_encoder_atomic_check(struct drm_encoder *encoder,
                                       struct drm_crtc_state *crtc_state,
                                       struct drm_connector_state *conn_state)
{
    struct nv_drm_connector *nv_connector =
        to_nv_connector(conn_state->connector);
    const struct nv_drm_connector_state *nv_conn_state =
        to_nv_drm_connector_state(conn_state);
    struct nv_drm_crtc_state *nv_crtc_state = to_nv_crtc_state(crtc_state);
    struct NvKmsKapiHeadModeSetConfig *config =
        &nv_crtc_state->req_config.modeSetConfig;
    const struct drm_display_mode *mode = &crtc_state->mode;
    const bool underscan =
        (nv_conn_state->underscan != NV_DRM_UNDERSCAN_OFF) &&
        ((nv_conn_state->underscan_hborder != 0) ||
         (nv_conn_state->underscan_vborder != 0));
    unsigned int scaling_mode = conn_state->scaling_mode;
    struct drm_display_mode native = { };
    bool scale = false;
    u32 raster_width, raster_height;
    u32 area_width, area_height;
    struct NvKmsRect out;

    /* DRM core only resets the adjusted mode in these cases */
    if (!crtc_state->mode_changed && !crtc_state->connectors_changed) {
        return 0;
    }

    config->viewPort.enabled = NV_FALSE;

    if (!crtc_state->enable ||
        (hweight32(crtc_state->connector_mask) != 1) ||
        (mode->flags & (DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_DBLSCAN)) ||
        (mode->hdisplay == 0) || (mode->vdisplay == 0)) {
        return 0;
    }

    /*
     * Scale modes smaller than the native size. Drive the native size as
     * requested, and leave larger modes to mode validation.
     */
    if ((scaling_mode != DRM_MODE_SCALE_NONE) &&
        nv_drm_connector_get_native_mode(nv_connector,
                                         drm_mode_vrefresh(mode),
                                         &native) &&
        (mode->hdisplay <= native.hdisplay) &&
        (mode->vdisplay <= native.vdisplay) &&
        ((mode->hdisplay != native.hdisplay) ||
         (mode->vdisplay != native.vdisplay))) {
        scale = true;
    }

    if (!scale && !underscan) {
        return 0;
    }

    raster_width = scale ? native.hdisplay : mode->hdisplay;
    raster_height = scale ? native.vdisplay : mode->vdisplay;
    area_width = raster_width;
    area_height = raster_height;

    if (underscan) {
        if ((2 * nv_conn_state->underscan_hborder >= raster_width) ||
            (2 * nv_conn_state->underscan_vborder >= raster_height)) {
            return -EINVAL;
        }
        area_width -= 2 * nv_conn_state->underscan_hborder;
        area_height -= 2 * nv_conn_state->underscan_vborder;
    }

    /* Underscan alone keeps the image's aspect ratio */
    if (!scale) {
        scaling_mode = DRM_MODE_SCALE_ASPECT;
    }

    switch (scaling_mode) {
    case DRM_MODE_SCALE_CENTER:
        out.width = min_t(u32, mode->hdisplay, area_width);
        out.height = min_t(u32, mode->vdisplay, area_height);
        break;
    case DRM_MODE_SCALE_ASPECT:
        if ((u32)mode->hdisplay * area_height >
            (u32)mode->vdisplay * area_width) {
            /* Wider than the area: fill the width */
            out.width = area_width;
            out.height = DIV_ROUND_CLOSEST((u32)mode->vdisplay * area_width,
                                           mode->hdisplay);
        } else {
            out.width = DIV_ROUND_CLOSEST((u32)mode->hdisplay * area_height,
                                          mode->vdisplay);
            out.height = area_height;
        }
        break;
    case DRM_MODE_SCALE_FULLSCREEN:
    default:
        out.width = area_width;
        out.height = area_height;
        break;
    }

    out.x = (raster_width - out.width) / 2;
    out.y = (raster_height - out.height) / 2;

    if (scale) {
        drm_mode_copy(&crtc_state->adjusted_mode, &native);
    }

    config->viewPort.enabled = NV_TRUE;
    config->viewPort.sizeIn.width = mode->hdisplay;
    config->viewPort.sizeIn.height = mode->vdisplay;
    config->viewPort.out = out;

    return 0;
}

static void nv_drm_encoder_prepare(struct drm_encoder *encoder)
{

}

static void nv_drm_encoder_commit(struct drm_encoder *encoder)
{

}

static void nv_drm_encoder_mode_set(struct drm_encoder *encoder,
                                    struct drm_display_mode *mode,
                                    struct drm_display_mode *adjusted_mode)
{

}

static const struct drm_encoder_helper_funcs nv_encoder_helper_funcs = {
    .atomic_check = nv_drm_encoder_atomic_check,
    .prepare    = nv_drm_encoder_prepare,
    .commit     = nv_drm_encoder_commit,
    .mode_set   = nv_drm_encoder_mode_set,
};

static uint32_t get_crtc_mask(struct drm_device *dev, uint32_t headMask)
{
    struct drm_crtc *crtc = NULL;
    uint32_t crtc_mask = 0x0;

    nv_drm_for_each_crtc(crtc, dev) {
        struct nv_drm_crtc *nv_crtc = to_nv_crtc(crtc);

        if (headMask & NVBIT(nv_crtc->head)) {
            crtc_mask |= drm_crtc_mask(crtc);
        }
    }

    return crtc_mask;
}

/*
 * Helper function to create new encoder for given NvKmsKapiDisplay
 * with given signal format.
 */
static struct drm_encoder*
nv_drm_encoder_new(struct drm_device *dev,
                   NvKmsKapiDisplay hDisplay,
                   NvKmsConnectorSignalFormat format,
                   unsigned int crtc_mask)
{
    struct nv_drm_device *nv_dev = to_nv_device(dev);

    struct nv_drm_encoder *nv_encoder = NULL;

    int ret = 0;

    /* Allocate an NVIDIA encoder object */

    nv_encoder = nv_drm_calloc(1, sizeof(*nv_encoder));

    if (nv_encoder == NULL) {
        NV_DRM_DEV_LOG_ERR(
            nv_dev,
            "Failed to allocate memory for NVIDIA-DRM encoder object");
        return ERR_PTR(-ENOMEM);
    }

    nv_encoder->hDisplay = hDisplay;

    /* Initialize the base encoder object and add it to the drm subsystem */

    ret = drm_encoder_init(dev,
                           &nv_encoder->base, &nv_encoder_funcs,
                           nvkms_connector_signal_to_drm_encoder_signal(format),
                           NULL);
    if (ret != 0) {
        nv_drm_free(nv_encoder);

        NV_DRM_DEV_LOG_ERR(
            nv_dev,
            "Failed to initialize encoder created from NvKmsKapiDisplay 0x%08x",
            hDisplay);
        return ERR_PTR(ret);
    }

    nv_encoder->base.possible_crtcs = crtc_mask;

    drm_encoder_helper_add(&nv_encoder->base, &nv_encoder_helper_funcs);

    return &nv_encoder->base;
}

/*
 * Add encoder for given NvKmsKapiDisplay
 */
struct drm_encoder*
nv_drm_add_encoder(struct drm_device *dev, NvKmsKapiDisplay hDisplay)
{
    struct nv_drm_device *nv_dev = to_nv_device(dev);

    struct NvKmsKapiStaticDisplayInfo *displayInfo = NULL;
    struct NvKmsKapiConnectorInfo *connectorInfo = NULL;

    struct drm_encoder *encoder = NULL;
    struct nv_drm_encoder *nv_encoder = NULL;

    struct drm_connector *connector = NULL;

    int ret = 0;

    /* Query NvKmsKapiStaticDisplayInfo and NvKmsKapiConnectorInfo */

    if ((displayInfo = nv_drm_calloc(1, sizeof(*displayInfo))) == NULL) {
        ret = -ENOMEM;
        goto done;
    }

    if (!nvKms->getStaticDisplayInfo(nv_dev->pDevice, hDisplay, displayInfo)) {
        ret = -EINVAL;
        goto done;
    }

    connectorInfo = nvkms_get_connector_info(nv_dev->pDevice,
                                             displayInfo->connectorHandle);

    if (IS_ERR(connectorInfo)) {
        ret = PTR_ERR(connectorInfo);
        goto done;
    }

    /* Create and add drm encoder */

    encoder = nv_drm_encoder_new(dev,
                                 displayInfo->handle,
                                 connectorInfo->signalFormat,
                                 get_crtc_mask(dev, displayInfo->headMask));

    if (IS_ERR(encoder)) {
        ret = PTR_ERR(encoder);
        goto done;
    }

    /* Get connector from respective physical index */

    connector =
        nv_drm_get_connector(dev,
                                 connectorInfo->physicalIndex,
                                 connectorInfo->type,
                                 displayInfo->internal, displayInfo->dpAddress);

    if (IS_ERR(connector)) {
        ret = PTR_ERR(connector);
        goto failed_connector_encoder_attach;
    }

    /* Attach encoder and connector */

    ret = nv_drm_connector_attach_encoder(connector, encoder);

    if (ret != 0) {
        NV_DRM_DEV_LOG_ERR(
            nv_dev,
            "Failed to attach encoder created from NvKmsKapiDisplay 0x%08x "
            "to connector",
            hDisplay);
        goto failed_connector_encoder_attach;
    }

    nv_encoder = to_nv_encoder(encoder);

    mutex_lock(&dev->mode_config.mutex);

    nv_encoder->nv_connector = to_nv_connector(connector);

    nv_drm_connector_mark_connection_status_dirty(nv_encoder->nv_connector);

    mutex_unlock(&dev->mode_config.mutex);

    goto done;

failed_connector_encoder_attach:

    drm_encoder_cleanup(encoder);

    nv_drm_free(encoder);

done:

    nv_drm_free(displayInfo);

    nv_drm_free(connectorInfo);

    return ret != 0 ? ERR_PTR(ret) : encoder;
}

static inline struct nv_drm_encoder*
get_nv_encoder_from_nvkms_display(struct drm_device *dev,
                                  NvKmsKapiDisplay hDisplay)
{
    struct drm_encoder *encoder;

    nv_drm_for_each_encoder(encoder, dev) {
        struct nv_drm_encoder *nv_encoder = to_nv_encoder(encoder);

        if (nv_encoder->hDisplay == hDisplay) {
            return nv_encoder;
        }
    }

    return NULL;
}

void nv_drm_handle_display_change(struct nv_drm_device *nv_dev,
                                  NvKmsKapiDisplay hDisplay)
{
    struct drm_device *dev = nv_dev->dev;
    struct nv_drm_encoder *nv_encoder = NULL;

    mutex_lock(&dev->mode_config.mutex);

    nv_encoder = get_nv_encoder_from_nvkms_display(dev, hDisplay);

    mutex_unlock(&dev->mode_config.mutex);

    if (nv_encoder == NULL) {
        return;
    }

    nv_drm_connector_mark_connection_status_dirty(nv_encoder->nv_connector);

    queue_delayed_work(nv_drm_percpu_wq, &nv_dev->hotplug_event_work, 0);
}

void nv_drm_handle_display_cp_change(struct nv_drm_device *nv_dev,
                                     NvKmsKapiDisplay hDisplay,
                                     enum NvKmsContentProtection cp)
{
    struct drm_device *dev = nv_dev->dev;
    struct nv_drm_encoder *nv_encoder = NULL;

    nv_encoder = get_nv_encoder_from_nvkms_display(dev, hDisplay);
    if (nv_encoder == NULL) {
        return;
    }

    mutex_lock(&dev->mode_config.mutex);

    nv_encoder->nv_connector->cp = cp;
    nv_drm_connector_update_content_protection(nv_encoder->nv_connector);

    mutex_unlock(&dev->mode_config.mutex);
}

void nv_drm_handle_display_cp_topology_change(struct nv_drm_device *nv_dev,
                                              NvKmsKapiDisplay hDisplay,
                                              const void *topology)
{
    struct drm_device *dev = nv_dev->dev;
    struct nv_drm_encoder *nv_encoder = NULL;

    mutex_lock(&dev->mode_config.mutex);

    nv_encoder = get_nv_encoder_from_nvkms_display(dev, hDisplay);
    if (nv_encoder == NULL) {
        NV_DRM_DEV_LOG_ERR(nv_dev, "Encoder not found for display %d", hDisplay);
        goto out;
    }
    nv_drm_connector_update_topology_property(nv_encoder->nv_connector, topology);

out:
    mutex_unlock(&dev->mode_config.mutex);
}

void nv_drm_handle_dynamic_display_connected(struct nv_drm_device *nv_dev,
                                             NvKmsKapiDisplay hDisplay)
{
    struct drm_device *dev = nv_dev->dev;

    struct drm_encoder *encoder = NULL;
    struct nv_drm_encoder *nv_encoder = NULL;

    /*
     * Look for an existing encoder with the same hDisplay and
     * use it if available.
     */

    nv_encoder = get_nv_encoder_from_nvkms_display(dev, hDisplay);

    if (nv_encoder != NULL) {
        NV_DRM_DEV_LOG_INFO(
            nv_dev,
            "Encoder with NvKmsKapiDisplay 0x%08x already exists.",
            hDisplay);
        return;
    }

    encoder = nv_drm_add_encoder(dev, hDisplay);

    if (IS_ERR(encoder)) {
        NV_DRM_DEV_LOG_ERR(
            nv_dev,
            "Failed to add encoder for NvKmsKapiDisplay 0x%08x",
            hDisplay);
        return;
    }

    queue_delayed_work(nv_drm_percpu_wq, &nv_dev->hotplug_event_work, 0);
}
#endif
