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

#include "nvidia-drm-helper.h"
#include "nvidia-drm-priv.h"
#include "nvidia-drm-connector.h"
#include "nvidia-drm-crtc.h"
#include "nvidia-drm-utils.h"
#include "nvidia-drm-encoder.h"
#include "nv_drm_common_ioctl.h"

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
#include <drm/drm_edid.h>
#include <drm/drm_sysfs.h>

#if defined(NV_DRM_CONNECTOR_FUNCS_HAVE_DEBUGFS_INIT) && defined(CONFIG_DEBUG_FS)
#include <drm/drm_drv.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#endif

/* Range of the "max bpc" property; NVKMS supports up to 12 bpc */
#define NV_DRM_MAX_BPC_MIN 8
#define NV_DRM_MAX_BPC_MAX 12

/*
 * Dithering support requires connector atomic_check, which we only enable on
 * kernels with HDR metadata support (v5.14+). This serves as a proxy for
 * "new enough kernel" with the new atomic_check signature that takes
 * drm_atomic_state.
 */
#if defined(NV_DRM_CONNECTOR_ATTACH_HDR_OUTPUT_METADATA_PROPERTY_PRESENT)
static int
nv_drm_connector_translate_dither_mode(enum nv_drm_dithering_mode mode,
                                       enum NvKmsDpyAttributeRequestedDitheringValue *state,
                                       enum NvKmsDpyAttributeRequestedDitheringModeValue *hw_mode)
{
    switch (mode) {
    case NV_DRM_DITHERING_MODE_AUTO:
        *state = NV_KMS_DPY_ATTRIBUTE_REQUESTED_DITHERING_AUTO;
        *hw_mode = NV_KMS_DPY_ATTRIBUTE_REQUESTED_DITHERING_MODE_AUTO;
        break;
    case NV_DRM_DITHERING_MODE_OFF:
        *state = NV_KMS_DPY_ATTRIBUTE_REQUESTED_DITHERING_DISABLED;
        *hw_mode = NV_KMS_DPY_ATTRIBUTE_REQUESTED_DITHERING_MODE_AUTO;
        break;
    case NV_DRM_DITHERING_MODE_STATIC_2X2:
        *state = NV_KMS_DPY_ATTRIBUTE_REQUESTED_DITHERING_ENABLED;
        *hw_mode = NV_KMS_DPY_ATTRIBUTE_REQUESTED_DITHERING_MODE_STATIC_2X2;
        break;
    case NV_DRM_DITHERING_MODE_DYNAMIC_2X2:
        *state = NV_KMS_DPY_ATTRIBUTE_REQUESTED_DITHERING_ENABLED;
        *hw_mode = NV_KMS_DPY_ATTRIBUTE_REQUESTED_DITHERING_MODE_DYNAMIC_2X2;
        break;
    case NV_DRM_DITHERING_MODE_TEMPORAL:
        *state = NV_KMS_DPY_ATTRIBUTE_REQUESTED_DITHERING_ENABLED;
        *hw_mode = NV_KMS_DPY_ATTRIBUTE_REQUESTED_DITHERING_MODE_TEMPORAL;
        break;
    case NV_DRM_DITHERING_MODE_ON:
        *state = NV_KMS_DPY_ATTRIBUTE_REQUESTED_DITHERING_ENABLED;
        *hw_mode = NV_KMS_DPY_ATTRIBUTE_REQUESTED_DITHERING_MODE_AUTO;
        break;
    default:
        return -EINVAL;
    }

    return 0;
}
#endif /* NV_DRM_CONNECTOR_ATTACH_HDR_OUTPUT_METADATA_PROPERTY_PRESENT */

static void nv_drm_connector_destroy(struct drm_connector *connector)
{
    struct nv_drm_connector *nv_connector = to_nv_connector(connector);

    drm_connector_unregister(connector);

    drm_connector_cleanup(connector);

    if (nv_connector->edid != NULL) {
        nv_drm_free(nv_connector->edid);
    }

    nv_drm_free(nv_connector);
}

#if !defined(NV_DRM_CONNECTOR_HAS_OVERRIDE_EDID)
static void nv_drm_connector_drop_probed_modes(struct drm_connector *connector)
{
    struct drm_display_mode *mode, *tmp;

    list_for_each_entry_safe(mode, tmp, &connector->probed_modes, head) {
        list_del(&mode->head);
        drm_mode_destroy(connector->dev, mode);
    }
}
#endif

static bool
__nv_drm_detect_encoder(struct NvKmsKapiDynamicDisplayParams *pDetectParams,
                        struct drm_connector *connector,
                        struct drm_encoder *encoder)
{
    struct nv_drm_connector *nv_connector = to_nv_connector(connector);
    struct drm_device *dev = connector->dev;
    struct nv_drm_device *nv_dev = to_nv_device(dev);
    struct nv_drm_encoder *nv_encoder;

    /*
     * DVI-I connectors can drive both digital and analog
     * encoders.  If a digital connection has been forced then
     * skip analog encoders.
     */

    if (connector->connector_type == DRM_MODE_CONNECTOR_DVII &&
        connector->force == DRM_FORCE_ON_DIGITAL &&
        encoder->encoder_type == DRM_MODE_ENCODER_DAC) {
        return false;
    }

    nv_encoder = to_nv_encoder(encoder);

    memset(pDetectParams, 0, sizeof(*pDetectParams));

    pDetectParams->handle = nv_encoder->hDisplay;

    switch (connector->force) {
        case DRM_FORCE_ON:
        case DRM_FORCE_ON_DIGITAL:
            pDetectParams->forceConnected = NV_TRUE;
            break;
        case DRM_FORCE_OFF:
            pDetectParams->forceDisconnected = NV_TRUE;
            break;
        case DRM_FORCE_UNSPECIFIED:
            break;
    }

#if defined(NV_DRM_CONNECTOR_HAS_OVERRIDE_EDID)
    if (connector->override_edid) {
#else
    nv_drm_connector_drop_probed_modes(connector);
    if (drm_edid_override_connector_update(connector) > 0) {
#endif
        const struct drm_property_blob *edid = connector->edid_blob_ptr;

        if (edid->length <= sizeof(pDetectParams->edid.buffer)) {
            memcpy(pDetectParams->edid.buffer, edid->data, edid->length);
            pDetectParams->edid.bufferSize = edid->length;
            pDetectParams->overrideEdid = NV_TRUE;
        } else {
            WARN_ON(edid->length >
                    sizeof(pDetectParams->edid.buffer));
        }
    }

    if (!nvKms->getDynamicDisplayInfo(nv_dev->pDevice, pDetectParams)) {
        NV_DRM_DEV_LOG_ERR(
            nv_dev,
            "Failed to detect display state");
        return false;
    }

    dev->mode_config.max_width  = pDetectParams->maxWidthInPixels;
    dev->mode_config.max_height = pDetectParams->maxHeightInPixels;

#if defined(NV_DRM_CONNECTOR_HAS_VRR_CAPABLE_PROPERTY)
    drm_connector_attach_vrr_capable_property(&nv_connector->base);
    drm_connector_set_vrr_capable_property(&nv_connector->base, pDetectParams->vrrSupported ? true : false);
#endif

    if (pDetectParams->connected) {
        if (!pDetectParams->overrideEdid && pDetectParams->edid.bufferSize) {

            if ((nv_connector->edid = nv_drm_calloc(
                        1,
                        pDetectParams->edid.bufferSize)) != NULL) {

                memcpy(nv_connector->edid,
                       pDetectParams->edid.buffer,
                       pDetectParams->edid.bufferSize);
            } else {
                NV_DRM_LOG_ERR("Out of Memory");
            }
        }

        return true;
    }

    return false;
}

static enum drm_connector_status __nv_drm_connector_detect_internal(
    struct drm_connector *connector)
{
    struct drm_device *dev = connector->dev;
    struct nv_drm_connector *nv_connector = to_nv_connector(connector);

    enum drm_connector_status status = connector_status_disconnected;

    struct drm_encoder *detected_encoder = NULL;
    struct nv_drm_encoder *nv_detected_encoder = NULL;
    struct drm_encoder *encoder;

    struct NvKmsKapiDynamicDisplayParams *pDetectParams = NULL;

    BUG_ON(!mutex_is_locked(&dev->mode_config.mutex));

    if (nv_connector->edid != NULL) {
        nv_drm_free(nv_connector->edid);
        nv_connector->edid = NULL;
    }

    if ((pDetectParams = nv_drm_calloc(
                1,
                sizeof(*pDetectParams))) == NULL) {
        WARN_ON(pDetectParams == NULL);
        goto done;
    }

    nv_drm_connector_for_each_possible_encoder(connector, encoder) {
        if (__nv_drm_detect_encoder(pDetectParams, connector, encoder)) {
            detected_encoder = encoder;
            break;
        }
    } nv_drm_connector_for_each_possible_encoder_end;

    if (detected_encoder == NULL) {
        goto done;
    }

    nv_detected_encoder = to_nv_encoder(detected_encoder);

    status = connector_status_connected;

    nv_connector->nv_detected_encoder = nv_detected_encoder;

    if (nv_connector->type == NVKMS_CONNECTOR_TYPE_DVI_I) {
        drm_object_property_set_value(
            &connector->base,
            dev->mode_config.dvi_i_subconnector_property,
            detected_encoder->encoder_type == DRM_MODE_ENCODER_DAC ?
                DRM_MODE_SUBCONNECTOR_DVIA :
                DRM_MODE_SUBCONNECTOR_DVID);
    }

done:

    nv_drm_free(pDetectParams);

    if (status == connector_status_disconnected &&
        nv_connector->modeset_permission_filep) {
        nv_drm_connector_revoke_permissions(dev, nv_connector);
    }

    return status;
}

#if defined(NV_DRM_CONNECTOR_FUNCS_HAS_ATOMIC_CREATE_STATE)
/**
 * nv_drm_connector_atomic_create_state - connector state creation hook
 * @crtc: DRM connector
 *
 * Allocate an empty DRM connector state.
 */
static struct drm_connector_state *
nv_drm_connector_atomic_create_state(struct drm_connector *connector)
{
    struct nv_drm_connector_state *nv_connector_state =
        nv_drm_calloc(1, sizeof(*nv_connector_state));

    if (!nv_connector_state) {
        return ERR_PTR(-ENOMEM);
    }

#if defined(NV_DRM_ATOMIC_HELPER_CONNECTOR_STATE_INIT)
    __drm_atomic_helper_connector_state_init(&nv_connector_state->base, connector);
#else
    __drm_atomic_helper_connector_state_reset(&nv_connector_state->base, connector);
#endif

    /* The state helpers leave it at 0, below the property's range */
    if (connector->max_bpc_property != NULL) {
        nv_connector_state->base.max_requested_bpc = NV_DRM_MAX_BPC_MAX;
    }

    return &nv_connector_state->base;
}
#else
static void nv_drm_connector_reset(struct drm_connector *connector)
{
    struct nv_drm_connector_state  * nv_connector_state =
            nv_drm_calloc(1, sizeof(*nv_connector_state));

    if (!nv_connector_state) {
        return;
    }

    if (connector->state)
    {
        struct nv_drm_connector_state *nv_drm_connector_state_old =
                to_nv_drm_connector_state(connector->state);
        __drm_atomic_helper_connector_destroy_state(connector->state);
        nv_drm_free(nv_drm_connector_state_old);
    }

    __drm_atomic_helper_connector_reset(connector, &nv_connector_state->base);

    /* The state helpers leave it at 0, below the property's range */
    if (connector->max_bpc_property != NULL) {
        nv_connector_state->base.max_requested_bpc = NV_DRM_MAX_BPC_MAX;
    }
}
#endif

static struct drm_connector_state* nv_drm_connector_atomic_duplicate_state(struct drm_connector *connector)
{
    struct nv_drm_connector_state *nv_drm_old_connector_state =
           to_nv_drm_connector_state(connector->state);
    struct nv_drm_connector_state *nv_drm_new_connector_state =
           nv_drm_calloc(1, sizeof(*nv_drm_new_connector_state));

    if (!nv_drm_new_connector_state) {
        return NULL;
    }

    __drm_atomic_helper_connector_duplicate_state(connector, &nv_drm_new_connector_state->base);

    nv_drm_new_connector_state->dithering_mode = nv_drm_old_connector_state->dithering_mode;
    nv_drm_new_connector_state->broadcast_rgb = nv_drm_old_connector_state->broadcast_rgb;
    nv_drm_new_connector_state->underscan = nv_drm_old_connector_state->underscan;
    nv_drm_new_connector_state->underscan_hborder =
        nv_drm_old_connector_state->underscan_hborder;
    nv_drm_new_connector_state->underscan_vborder =
        nv_drm_old_connector_state->underscan_vborder;
    nv_drm_new_connector_state->hdcp_topology_blob = nv_drm_old_connector_state->hdcp_topology_blob;
    if (nv_drm_new_connector_state->hdcp_topology_blob) {
        drm_property_blob_get(nv_drm_new_connector_state->hdcp_topology_blob);
    }

    nv_drm_new_connector_state->hdmi_vsif_metadata =
        nv_drm_old_connector_state->hdmi_vsif_metadata;
    if (nv_drm_new_connector_state->hdmi_vsif_metadata) {
        drm_property_blob_get(nv_drm_new_connector_state->hdmi_vsif_metadata);
    }

    return &nv_drm_new_connector_state->base;
}

static void nv_drm_connector_atomic_destroy_state(
            struct drm_connector *connector,
            struct drm_connector_state *state)
{
    struct nv_drm_connector_state *nv_drm_connector_state =
           to_nv_drm_connector_state(state);

    __drm_atomic_helper_connector_destroy_state(state);
    drm_property_blob_put(nv_drm_connector_state->hdcp_topology_blob);

    drm_property_blob_put(nv_drm_connector_state->hdmi_vsif_metadata);

    nv_drm_free(nv_drm_connector_state);
}

static void __nv_drm_connector_force(struct drm_connector *connector)
{
    __nv_drm_connector_detect_internal(connector);
}

static enum drm_connector_status
nv_drm_connector_detect(struct drm_connector *connector, bool force)
{
    return __nv_drm_connector_detect_internal(connector);
}

static int nv_drm_connector_atomic_set_property(
    struct drm_connector *connector,
    struct drm_connector_state *state,
    struct drm_property *property,
    uint64_t val)
{
    struct nv_drm_device *nv_dev = to_nv_device(connector->dev);
    struct nv_drm_connector_state *nv_connector_state =
        to_nv_drm_connector_state(state);

    if (property == nv_dev->nv_connector_dithering_mode_property) {
        if (val >= NV_DRM_DITHERING_MODE_MAX) {
            return -EINVAL;
        }
        nv_connector_state->dithering_mode = val;
        return 0;
    } else if (property == nv_dev->nv_connector_broadcast_rgb_property) {
        if (val >= NV_DRM_BROADCAST_RGB_MAX) {
            return -EINVAL;
        }
        nv_connector_state->broadcast_rgb = val;
        return 0;
    } else if (property == nv_dev->nv_connector_underscan_property) {
        if (val >= NV_DRM_UNDERSCAN_MAX) {
            return -EINVAL;
        }
        nv_connector_state->underscan = val;
        return 0;
    } else if (property == nv_dev->nv_connector_underscan_hborder_property) {
        nv_connector_state->underscan_hborder = val;
        return 0;
    } else if (property == nv_dev->nv_connector_underscan_vborder_property) {
        nv_connector_state->underscan_vborder = val;
        return 0;
    } else if (property == nv_dev->nv_hdcp_level_property) {
        // nv_hdcp_level_property is read only
        return -EINVAL;
    }
    

    if (property == nv_dev->nv_connector_hdmi_vsif_metadata_property) {
        return nv_drm_atomic_replace_property_blob_from_id_size_range(
                    nv_dev->dev,
                    &nv_connector_state->hdmi_vsif_metadata,
                    val,
                    NV_DRM_HDMI_VSIF_METADATA_MIN_PAYLOAD_SIZE,
                    NV_DRM_HDMI_VSIF_METADATA_MAX_PAYLOAD_SIZE);
    }

    /* Unknown property - DRM core handles standard connector properties */
    return -EINVAL;
}

static int nv_drm_connector_atomic_get_property(
    struct drm_connector *connector,
    const struct drm_connector_state *state,
    struct drm_property *property,
    uint64_t *val)
{
    struct nv_drm_device *nv_dev = to_nv_device(connector->dev);
    struct nv_drm_connector *nv_conn = to_nv_connector(connector);    
    const struct nv_drm_connector_state *nv_connector_state =
        to_nv_drm_connector_state_const(state);

    if (property == nv_dev->nv_connector_dithering_mode_property) {
        *val = nv_connector_state->dithering_mode;
        return 0;
    } else if (property == nv_dev->nv_connector_broadcast_rgb_property) {
        *val = nv_connector_state->broadcast_rgb;
        return 0;
    } else if (property == nv_dev->nv_connector_underscan_property) {
        *val = nv_connector_state->underscan;
        return 0;
    } else if (property == nv_dev->nv_connector_underscan_hborder_property) {
        *val = nv_connector_state->underscan_hborder;
        return 0;
    } else if (property == nv_dev->nv_connector_underscan_vborder_property) {
        *val = nv_connector_state->underscan_vborder;
        return 0;
    } else if (property == nv_dev->nv_hdcp_level_property) {
        *val = nv_conn->cp;
        return 0;
    }

    if (property == nv_dev->nv_connector_hdmi_vsif_metadata_property) {
        *val = nv_connector_state->hdmi_vsif_metadata ?
            nv_connector_state->hdmi_vsif_metadata->base.id : 0;
        return 0;
    }

    /* Unknown property - DRM core handles standard connector properties */
    return -EINVAL;
}

#if defined(NV_DRM_CONNECTOR_FUNCS_HAVE_DEBUGFS_INIT) && defined(CONFIG_DEBUG_FS)
/*
 * current_bpc: the bpc NVKMS currently drives the display with, the bpc
 * limit NVKMS has stored for it from "max bpc" (0 for none), and whether
 * that limit lowered the bpc. DRM core's output_bpc file next to it shows the
 * maximum the display supports.
 */
static int nv_drm_connector_current_bpc_show(struct seq_file *m, void *data)
{
    struct drm_connector *connector = m->private;
    struct drm_device *dev = connector->dev;
    struct nv_drm_device *nv_dev = to_nv_device(dev);
    struct nv_drm_connector *nv_connector = to_nv_connector(connector);
    NvU32 bpc = 0;
    NvU32 max_bpc = 0;
    NvBool limited = NV_FALSE;
    int idx;
    int ret = 0;

    /* The NVKMS device is freed once the DRM device is unplugged */
    if (!drm_dev_enter(dev, &idx)) {
        return -ENODEV;
    }

    drm_modeset_lock(&dev->mode_config.connection_mutex, NULL);

    if ((connector->state->crtc != NULL) &&
        (nv_connector->nv_detected_encoder != NULL) &&
        !nvKms->getDisplayColorBpc(nv_dev->pDevice,
                                   nv_connector->nv_detected_encoder->hDisplay,
                                   &bpc, &max_bpc, &limited)) {
        ret = -EIO;
    }

    drm_modeset_unlock(&dev->mode_config.connection_mutex);

    drm_dev_exit(idx);

    if (ret == 0) {
        seq_printf(m, "Current: %u\n", bpc);
        seq_printf(m, "Maximum: %u\n", max_bpc);
        seq_printf(m, "Limited: %s\n", limited ? "yes" : "no");
    }

    return ret;
}
DEFINE_SHOW_ATTRIBUTE(nv_drm_connector_current_bpc);

static void nv_drm_connector_debugfs_init(struct drm_connector *connector,
                                          struct dentry *root)
{
    debugfs_create_file("current_bpc", 0444, root, connector,
                        &nv_drm_connector_current_bpc_fops);
}
#endif

static struct drm_connector_funcs nv_connector_funcs = {
    .destroy                = nv_drm_connector_destroy,
#if defined(NV_DRM_CONNECTOR_FUNCS_HAS_ATOMIC_CREATE_STATE)
    .atomic_create_state    = nv_drm_connector_atomic_create_state,
#else
    .reset                  = nv_drm_connector_reset,
#endif
    .force                  = __nv_drm_connector_force,
    .detect                 = nv_drm_connector_detect,
    .fill_modes             = drm_helper_probe_single_connector_modes,
    .atomic_duplicate_state = nv_drm_connector_atomic_duplicate_state,
    .atomic_destroy_state   = nv_drm_connector_atomic_destroy_state,
    .atomic_set_property    = nv_drm_connector_atomic_set_property,
    .atomic_get_property    = nv_drm_connector_atomic_get_property,
#if defined(NV_DRM_CONNECTOR_FUNCS_HAVE_DEBUGFS_INIT) && defined(CONFIG_DEBUG_FS)
    .debugfs_init           = nv_drm_connector_debugfs_init,
#endif
};

/*
 * Remember the modes at the native resolution: the size of the preferred
 * mode, or the largest size if the display has no preferred mode.
 */
static void nv_drm_connector_save_native_mode(
    struct nv_drm_connector *nv_connector,
    const struct drm_display_mode *mode)
{
    const bool preferred = (mode->type & DRM_MODE_TYPE_PREFERRED) != 0;

    spin_lock(&nv_connector->native_modes_lock);

    if (nv_connector->num_native_modes > 0) {
        const struct drm_display_mode *native = &nv_connector->native_modes[0];
        const bool same_size = (mode->hdisplay == native->hdisplay) &&
                               (mode->vdisplay == native->vdisplay);
        const bool larger =
            ((u32)mode->hdisplay * mode->vdisplay) >
            ((u32)native->hdisplay * native->vdisplay);

        if (!same_size) {
            if (nv_connector->native_mode_preferred ||
                (!preferred && !larger)) {
                goto done;
            }
            nv_connector->num_native_modes = 0;
        }
    }

    if (preferred) {
        nv_connector->native_mode_preferred = true;
    }

    if (nv_connector->num_native_modes < NV_DRM_CONNECTOR_MAX_NATIVE_MODES) {
        drm_mode_copy(
            &nv_connector->native_modes[nv_connector->num_native_modes++],
            mode);
    }

done:
    spin_unlock(&nv_connector->native_modes_lock);
}

static void nv_drm_connector_clear_native_modes(
    struct nv_drm_connector *nv_connector)
{
    spin_lock(&nv_connector->native_modes_lock);
    nv_connector->num_native_modes = 0;
    nv_connector->native_mode_preferred = false;
    spin_unlock(&nv_connector->native_modes_lock);
}

/*
 * Find the native-resolution mode with the refresh rate closest to vrefresh.
 * Returns false if the display has none (not probed yet).
 */
bool nv_drm_connector_get_native_mode(struct nv_drm_connector *nv_connector,
                                      int vrefresh,
                                      struct drm_display_mode *native_mode)
{
    unsigned int best = 0;
    int best_diff = INT_MAX;
    unsigned int i;
    bool found;

    spin_lock(&nv_connector->native_modes_lock);

    for (i = 0; i < nv_connector->num_native_modes; i++) {
        const int diff =
            abs(drm_mode_vrefresh(&nv_connector->native_modes[i]) - vrefresh);

        if (diff < best_diff) {
            best_diff = diff;
            best = i;
        }
    }

    found = nv_connector->num_native_modes > 0;
    if (found) {
        drm_mode_copy(native_mode, &nv_connector->native_modes[best]);
    }

    spin_unlock(&nv_connector->native_modes_lock);

    return found;
}

static int nv_drm_connector_get_modes(struct drm_connector *connector)
{
    struct drm_device *dev = connector->dev;
    struct nv_drm_device *nv_dev = to_nv_device(dev);
    struct nv_drm_connector *nv_connector = to_nv_connector(connector);
    struct nv_drm_encoder *nv_detected_encoder =
                           nv_connector->nv_detected_encoder;
    NvU32 modeIndex = 0;
    int   count = 0;


    if (nv_connector->edid != NULL) {
        nv_drm_connector_update_edid_property(connector, nv_connector->edid);
    }

    nv_drm_connector_clear_native_modes(nv_connector);

    while (1) {
        struct drm_display_mode *mode;
        struct NvKmsKapiDisplayMode displayMode;
        NvBool valid = 0;
        NvBool preferredMode = NV_FALSE;
        int ret;

        ret = nvKms->getDisplayMode(nv_dev->pDevice,
                                    nv_detected_encoder->hDisplay,
                                    modeIndex++, &displayMode, &valid,
                                    &preferredMode);

        if (ret < 0) {
            NV_DRM_DEV_LOG_ERR(
                nv_dev,
                "Failed to get mode at modeIndex %d of NvKmsKapiDisplay 0x%08x",
                modeIndex, nv_detected_encoder->hDisplay);
            break;
        }

        /* Is end of mode-list */

        if (ret == 0) {
            break;
        }

        /* Ignore invalid modes */

        if (!valid) {
            continue;
        }

        mode = drm_mode_create(connector->dev);

        if (mode == NULL) {
            NV_DRM_DEV_LOG_ERR(
                nv_dev,
                "Failed to create mode for NvKmsKapiDisplay 0x%08x",
                nv_detected_encoder->hDisplay);
            continue;
        }

        nvkms_display_mode_to_drm_mode(&displayMode, mode);

        if (preferredMode) {
            mode->type |= DRM_MODE_TYPE_PREFERRED;
        }

        nv_drm_connector_save_native_mode(nv_connector, mode);

        /* Add a mode to a connector's probed_mode list */

        drm_mode_probed_add(connector, mode);

        count++;
    }

    return count;
}

static enum drm_mode_status nv_drm_connector_mode_valid(struct drm_connector    *connector,
#if defined(NV_DRM_CONNECTOR_HELPER_FUNCS_MODE_VALID_HAS_CONST_MODE_ARG)
                                                        const struct drm_display_mode *mode)
#else
                                                        struct drm_display_mode *mode)
#endif
{
    struct drm_device *dev = connector->dev;
    struct nv_drm_device *nv_dev = to_nv_device(dev);
    struct nv_drm_encoder *nv_detected_encoder =
                           to_nv_connector(connector)->nv_detected_encoder;
    struct NvKmsKapiDisplayMode displayMode;

    if (nv_detected_encoder == NULL) {
        return MODE_BAD;
    }

    drm_mode_to_nvkms_display_mode(mode, &displayMode);

    if (!nvKms->validateDisplayMode(nv_dev->pDevice,
                                    nv_detected_encoder->hDisplay,
                                    &displayMode)) {
        return MODE_BAD;
    }

    return MODE_OK;
}

static struct drm_encoder*
nv_drm_connector_best_encoder(struct drm_connector *connector)
{
    struct nv_drm_connector *nv_connector = to_nv_connector(connector);

    if (nv_connector->nv_detected_encoder != NULL) {
        return &nv_connector->nv_detected_encoder->base;
    }

    return NULL;
}

#if defined(NV_DRM_MODE_CREATE_DP_COLORSPACE_PROPERTY_HAS_SUPPORTED_COLORSPACES_ARG)
static const NvU32 __nv_drm_connector_supported_colorspaces =
    BIT(DRM_MODE_COLORIMETRY_BT2020_RGB) |
    BIT(DRM_MODE_COLORIMETRY_BT2020_YCC);
#endif

#if defined(NV_DRM_CONNECTOR_ATTACH_HDR_OUTPUT_METADATA_PROPERTY_PRESENT)
/* NVKMS drives 8, 10 or 12 bpc; round a "max bpc" value down to one of them */
static NvU8 nv_drm_connector_round_max_bpc(unsigned int max_bpc)
{
    return (max_bpc >= 12) ? 12 : (max_bpc >= 10) ? 10 : 8;
}

/*
 * A "max bpc" change only needs a modeset if it changes the bpc NVKMS drives
 * the display with. When the new limit is still at least the current bpc, the
 * next modeset would pick the same bpc, so NVKMS only has to store the limit,
 * which a flip does as well. Unless the limit is raised while the old limit
 * held the bpc down: then a higher bpc may be possible. If something else
 * held it down, like the link bandwidth, a higher limit doesn't change it.
 *
 * DRM core flags the CRTC's connectors as changed for any "max bpc" change.
 * Only skip the modeset if that is the only reason: same CRTC, encoder, link
 * status and color format, and the CRTC drives just this connector and stays
 * active without other modeset changes.
 */
static bool
nv_drm_connector_max_bpc_needs_modeset(struct drm_connector *connector,
                                       nv_drm_atomic_state_base_t *state,
                                       struct drm_crtc_state *crtc_state)
{
    struct nv_drm_device *nv_dev = to_nv_device(connector->dev);
    struct nv_drm_connector *nv_connector = to_nv_connector(connector);
    const struct drm_connector_state *old_connector_state =
        drm_atomic_get_old_connector_state(state, connector);
    const struct drm_connector_state *new_connector_state =
        drm_atomic_get_new_connector_state(state, connector);
    const struct drm_crtc_state *old_crtc_state =
        drm_atomic_get_old_crtc_state(state, crtc_state->crtc);
    const NvU8 old_max_bpc =
        nv_drm_connector_round_max_bpc(old_connector_state->max_requested_bpc);
    const NvU8 new_max_bpc =
        nv_drm_connector_round_max_bpc(new_connector_state->max_requested_bpc);
    NvU32 bpc = 0;
    NvU32 stored_max_bpc = 0;
    NvBool limited = NV_FALSE;

    if (crtc_state->mode_changed ||
        crtc_state->active_changed ||
        !crtc_state->active ||
        (old_connector_state->crtc != new_connector_state->crtc) ||
        (old_connector_state->best_encoder !=
         new_connector_state->best_encoder) ||
        (old_connector_state->link_status !=
         new_connector_state->link_status) ||
#if defined(NV_DRM_CONNECTOR_STATE_HAS_COLOR_FORMAT)
        (old_connector_state->color_format !=
         new_connector_state->color_format) ||
#endif
        (old_crtc_state->connector_mask != crtc_state->connector_mask) ||
        (hweight32(crtc_state->connector_mask) != 1) ||
        (nv_connector->nv_detected_encoder == NULL)) {
        return true;
    }

    /*
     * Commits reach NVKMS before the commit ioctl returns, so NVKMS drives
     * the display as in the old state; it must also have stored the old
     * limit.
     */
    if (!nvKms->getDisplayColorBpc(nv_dev->pDevice,
                                   nv_connector->nv_detected_encoder->hDisplay,
                                   &bpc, &stored_max_bpc, &limited)) {
        return true;
    }

    return (stored_max_bpc != old_max_bpc) ||
           (bpc == 0) ||
           (bpc > new_max_bpc) ||
           ((new_max_bpc > old_max_bpc) && limited);
}

static int
__nv_drm_connector_atomic_check(struct drm_connector *connector,
                                nv_drm_atomic_state_base_t *state)
{
    struct drm_connector_state *new_connector_state =
        drm_atomic_get_new_connector_state(state, connector);
    struct drm_connector_state *old_connector_state =
        drm_atomic_get_old_connector_state(state, connector);
    struct nv_drm_device *nv_dev = to_nv_device(connector->dev);
    struct nv_drm_connector_state *nv_new_connector_state =
        to_nv_drm_connector_state(new_connector_state);
    struct nv_drm_connector_state *nv_old_connector_state =
        to_nv_drm_connector_state(old_connector_state);

    struct drm_crtc *crtc = new_connector_state->crtc;
    struct drm_crtc_state *crtc_state;
    struct nv_drm_crtc_state *nv_crtc_state;
    struct NvKmsKapiHeadRequestedConfig *req_config;

    if (!crtc) {
        return 0;
    }

    crtc_state = drm_atomic_get_new_crtc_state(state, crtc);
    nv_crtc_state = to_nv_crtc_state(crtc_state);
    req_config = &nv_crtc_state->req_config;

    /* Handle dithering changes */
    req_config->flags.ditheringChanged =
        nv_old_connector_state->dithering_mode !=
        nv_new_connector_state->dithering_mode;
    if (req_config->flags.ditheringChanged) {
        enum NvKmsDpyAttributeRequestedDitheringValue state;
        enum NvKmsDpyAttributeRequestedDitheringModeValue mode;
        int ret = nv_drm_connector_translate_dither_mode(
            nv_new_connector_state->dithering_mode, &state, &mode);
        if (ret != 0) {
            return ret;
        }
        req_config->modeSetConfig.dithering.state = state;
        req_config->modeSetConfig.dithering.mode = mode;
    }

    /*
     * Handle Broadcast RGB changes. The output color range is only applied by
     * a modeset, like on the drivers using the DRM HDMI state helpers.
     * "Automatic" is the NVKMS default, full range.
     */
    if (nv_old_connector_state->broadcast_rgb !=
        nv_new_connector_state->broadcast_rgb) {
        req_config->modeSetConfig.limitedColorRange =
            (nv_new_connector_state->broadcast_rgb ==
             NV_DRM_BROADCAST_RGB_LIMITED);
        req_config->flags.colorRangeChanged = NV_TRUE;
        crtc_state->mode_changed = true;
    }

    /*
     * Handle "max bpc". Like the color range it is only applied by a
     * modeset; DRM core already flags the CRTC when it changes. Pass it along
     * with every modeset, so it also reaches a display that is newly driven
     * by this CRTC or was recreated by a hotplug. A change that keeps the
     * current bpc is only stored, without a modeset.
     */
    if (connector->max_bpc_property != NULL) {
        const NvBool maxBpcChanged =
            old_connector_state->max_requested_bpc !=
            new_connector_state->max_requested_bpc;

        if (maxBpcChanged || drm_atomic_crtc_needs_modeset(crtc_state)) {
            req_config->modeSetConfig.maxBpc =
                nv_drm_connector_round_max_bpc(
                    new_connector_state->max_requested_bpc);
            req_config->flags.maxBpcChanged = NV_TRUE;
        }
        if (maxBpcChanged) {
            if (nv_drm_connector_max_bpc_needs_modeset(connector, state,
                                                       crtc_state)) {
                crtc_state->mode_changed = true;
            } else {
                crtc_state->connectors_changed = false;
            }
        }
    }

    /*
     * Handle "scaling mode" and underscan. They decide the raster timings
     * and the viewport the encoder check derives from the mode, so they need
     * a modeset.
     */
    if ((old_connector_state->scaling_mode !=
         new_connector_state->scaling_mode) ||
        (nv_old_connector_state->underscan !=
         nv_new_connector_state->underscan) ||
        (nv_old_connector_state->underscan_hborder !=
         nv_new_connector_state->underscan_hborder) ||
        (nv_old_connector_state->underscan_vborder !=
         nv_new_connector_state->underscan_vborder)) {
        crtc_state->mode_changed = true;
    }

    /*
     * Handle "content type". It only changes the AVI InfoFrame, so unlike
     * the properties above it doesn't need a modeset; compositors set it in
     * their page flip commits. Pass it with every modeset too, so it also
     * reaches a display that is newly driven by this CRTC or was recreated
     * by a hotplug. The DRM values are the NVKMS values minus one (NVKMS 0
     * keeps its own default).
     */
    if ((connector->dev->mode_config.content_type_property != NULL) &&
        ((old_connector_state->content_type !=
          new_connector_state->content_type) ||
         drm_atomic_crtc_needs_modeset(crtc_state))) {
        req_config->modeSetConfig.contentType =
            new_connector_state->content_type + 1;
        req_config->flags.contentTypeChanged = NV_TRUE;
    }

    /* Handle HDMI VSIF metadata changes */
    req_config->flags.hdmiVsifMetadataChanged =
        !nv_drm_blobs_equal(nv_old_connector_state->hdmi_vsif_metadata,
                            nv_new_connector_state->hdmi_vsif_metadata);
    if (req_config->flags.hdmiVsifMetadataChanged) {
        if (nv_new_connector_state->hdmi_vsif_metadata &&
            nv_new_connector_state->hdmi_vsif_metadata->data) {
            struct drm_nvidia_hdmi_vsif_metadata* vsif_metadata =
                (struct drm_nvidia_hdmi_vsif_metadata*)
                nv_new_connector_state->hdmi_vsif_metadata->data;
            NvU32 payload_size =
                nv_new_connector_state->hdmi_vsif_metadata->length;
            WARN_ON(payload_size < NV_DRM_HDMI_VSIF_METADATA_MIN_PAYLOAD_SIZE ||
                    payload_size > NV_DRM_HDMI_VSIF_METADATA_MAX_PAYLOAD_SIZE);
            memcpy(req_config->modeSetConfig.hdmiVsifMetadata.payload,
                   vsif_metadata->payload,
                   payload_size);
            req_config->modeSetConfig.hdmiVsifMetadata.payloadSize =
                payload_size;
        } else {
            req_config->modeSetConfig.hdmiVsifMetadata.payloadSize = 0;
        }
    }

    /*
     * Override metadata for the entire head instead of allowing NVKMS to derive
     * it from the layers' metadata.
     *
     * This is the metadata that will sent to the display, and if applicable,
     * layers will be tone mapped to this metadata rather than that of the
     * display.
     */
    req_config->flags.hdrInfoFrameChanged =
        !drm_connector_atomic_hdr_metadata_equal(old_connector_state,
                                                 new_connector_state);
    if (new_connector_state->hdr_output_metadata &&
        new_connector_state->hdr_output_metadata->data) {

        /*
         * Note that HDMI definitions are used here even though we might not
         * be using HDMI. While that seems odd, it is consistent with
         * upstream behavior.
         */

        struct hdr_output_metadata *hdr_metadata =
            new_connector_state->hdr_output_metadata->data;
        struct hdr_metadata_infoframe *info_frame =
            &hdr_metadata->hdmi_metadata_type1;
        unsigned int i;

        if (hdr_metadata->metadata_type != HDMI_STATIC_METADATA_TYPE1) {
            return -EINVAL;
        }

        for (i = 0; i < ARRAY_SIZE(info_frame->display_primaries); i++) {
            req_config->modeSetConfig.hdrInfoFrame.staticMetadata.displayPrimaries[i].x =
                info_frame->display_primaries[i].x;
            req_config->modeSetConfig.hdrInfoFrame.staticMetadata.displayPrimaries[i].y =
                info_frame->display_primaries[i].y;
        }

        req_config->modeSetConfig.hdrInfoFrame.staticMetadata.whitePoint.x =
            info_frame->white_point.x;
        req_config->modeSetConfig.hdrInfoFrame.staticMetadata.whitePoint.y =
            info_frame->white_point.y;
        req_config->modeSetConfig.hdrInfoFrame.staticMetadata.maxDisplayMasteringLuminance =
            info_frame->max_display_mastering_luminance;
        req_config->modeSetConfig.hdrInfoFrame.staticMetadata.minDisplayMasteringLuminance =
            info_frame->min_display_mastering_luminance;
        req_config->modeSetConfig.hdrInfoFrame.staticMetadata.maxCLL =
            info_frame->max_cll;
        req_config->modeSetConfig.hdrInfoFrame.staticMetadata.maxFALL =
            info_frame->max_fall;

        req_config->modeSetConfig.hdrInfoFrame.eotf = info_frame->eotf;

        req_config->modeSetConfig.hdrInfoFrame.enabled = NV_TRUE;
    } else {
        req_config->modeSetConfig.hdrInfoFrame.enabled = NV_FALSE;
    }

    req_config->flags.colorimetryChanged =
        (old_connector_state->colorspace != new_connector_state->colorspace);
    // When adding a case here, also add to __nv_drm_connector_supported_colorspaces
    switch (new_connector_state->colorspace) {
        case DRM_MODE_COLORIMETRY_DEFAULT:
            req_config->modeSetConfig.colorimetry =
                NVKMS_OUTPUT_COLORIMETRY_DEFAULT;
            break;
        case DRM_MODE_COLORIMETRY_BT601_YCC:
            req_config->modeSetConfig.colorimetry =
                NVKMS_OUTPUT_COLORIMETRY_BT601;
            break;
        case DRM_MODE_COLORIMETRY_BT709_YCC:
            req_config->modeSetConfig.colorimetry =
                NVKMS_OUTPUT_COLORIMETRY_BT709;
            break;
        case DRM_MODE_COLORIMETRY_BT2020_RGB:
        case DRM_MODE_COLORIMETRY_BT2020_YCC:
            // Ignore RGB/YCC
            // See https://patchwork.freedesktop.org/patch/525496/?series=111865&rev=4
            req_config->modeSetConfig.colorimetry =
                NVKMS_OUTPUT_COLORIMETRY_BT2100;
            break;
        default:
            // XXX HDR TODO: Add support for more color spaces
            NV_DRM_DEV_LOG_ERR(nv_dev, "Unsupported color space");
            return -EINVAL;
    }

    return 0;
}
#endif /* defined(NV_DRM_CONNECTOR_ATTACH_HDR_OUTPUT_METADATA_PROPERTY_PRESENT) */

static const struct drm_connector_helper_funcs nv_connector_helper_funcs = {
    .get_modes    = nv_drm_connector_get_modes,
    .mode_valid   = nv_drm_connector_mode_valid,
    .best_encoder = nv_drm_connector_best_encoder,
#if defined(NV_DRM_CONNECTOR_ATTACH_HDR_OUTPUT_METADATA_PROPERTY_PRESENT)
    .atomic_check = __nv_drm_connector_atomic_check,
#endif
};

/*
 * Set the standard PATH property of a DisplayPort MST connector, in the form
 * DRM's MST helpers use: "mst:<root connector id>-<port>-...-<port>".
 * Compositors identify MST outputs by it, so that their settings survive
 * replugging a dock. NVKMS names an MST display by the port numbers along
 * its path from the root branch, separated by '.', e.g. "1.8"; the root
 * connector is the one of the same physical connector without an address.
 */
static void nv_drm_connector_set_mst_path(struct nv_drm_connector *nv_connector)
{
    struct drm_device *dev = nv_connector->base.dev;
    struct nv_drm_device *nv_dev = to_nv_device(dev);
    struct drm_connector *connector;
    struct drm_connector_list_iter conn_iter;
    char path[16 + NVKMS_DP_ADDRESS_STRING_LENGTH];
    const char *p;
    int root_id = -1;
    size_t len;

    if ((nv_connector->dpAddress[0] == '\0') ||
        (dev->mode_config.path_property == NULL)) {
        return;
    }

    drm_connector_list_iter_begin(dev, &conn_iter);
    drm_for_each_connector_iter(connector, &conn_iter) {
        const struct nv_drm_connector *root = to_nv_connector(connector);

        if ((root->physicalIndex == nv_connector->physicalIndex) &&
            (root->dpAddress[0] == '\0')) {
            root_id = connector->base.id;
            break;
        }
    }
    drm_connector_list_iter_end(&conn_iter);

    if (root_id < 0) {
        return;
    }

    len = scnprintf(path, sizeof(path), "mst:%d-", root_id);
    for (p = nv_connector->dpAddress;
         (*p != '\0') && (len < sizeof(path) - 1); p++) {
        path[len++] = (*p == '.') ? '-' : *p;
    }
    path[len] = '\0';

    drm_object_attach_property(&nv_connector->base.base,
                               dev->mode_config.path_property, 0);

    if (drm_connector_set_path_property(&nv_connector->base, path) != 0) {
        NV_DRM_DEV_LOG_ERR(
            nv_dev,
            "Failed to set the path property of connector created from physical index %u",
            nv_connector->physicalIndex);
    }
}

static struct drm_connector*
nv_drm_connector_new(struct drm_device *dev,
                     NvU32 physicalIndex, NvKmsConnectorType type,
                     NvBool internal,
                     char dpAddress[NVKMS_DP_ADDRESS_STRING_LENGTH])
{
    struct nv_drm_device *nv_dev = to_nv_device(dev);
    struct nv_drm_connector *nv_connector = NULL;
    struct nv_drm_connector_state *nv_connector_state = NULL;
    int ret = -ENOMEM;

    if ((nv_connector = nv_drm_calloc(1, sizeof(*nv_connector))) == NULL) {
        goto failed;
    }

    if ((nv_connector_state =
            nv_drm_calloc(1, sizeof(*nv_connector_state))) == NULL) {
        goto failed_state_alloc;
    }

    nv_connector->base.state = &nv_connector_state->base;
    nv_connector->base.state->connector = &nv_connector->base;

    nv_connector->physicalIndex = physicalIndex;
    nv_connector->type     = type;
    nv_connector->internal = internal;
    nv_connector->modeset_permission_filep = NULL;
    nv_connector->modeset_permission_crtc = NULL;
    spin_lock_init(&nv_connector->native_modes_lock);
    nv_connector->cp = NVKMS_CONTENT_PROTECTION_OFF;

    strcpy(nv_connector->dpAddress, dpAddress);

    ret = drm_connector_init(
        dev,
        &nv_connector->base, &nv_connector_funcs,
        nvkms_connector_type_to_drm_connector_type(type, internal));

    if (ret != 0) {
        NV_DRM_DEV_LOG_ERR(
            nv_dev,
            "Failed to initialize connector created from physical index %u",
            nv_connector->physicalIndex);
        goto failed_connector_init;
    }

    drm_connector_helper_add(&nv_connector->base, &nv_connector_helper_funcs);

    nv_connector->base.polled = DRM_CONNECTOR_POLL_HPD;

    if (nv_connector->type == NVKMS_CONNECTOR_TYPE_VGA) {
        nv_connector->base.polled =
            DRM_CONNECTOR_POLL_CONNECT | DRM_CONNECTOR_POLL_DISCONNECT;
    }

#ifdef NV_DRM_SUPPORT_CONTENT_PROTECTION_PROPERTY
    /* attach content protection properties */
    if ((nv_connector->type == NVKMS_CONNECTOR_TYPE_DP) ||
        (nv_connector->type == NVKMS_CONNECTOR_TYPE_HDMI)) {
        ret = drm_connector_attach_content_protection_property(&nv_connector->base, true);
        if (ret != 0) {
            NV_DRM_DEV_LOG_ERR(
                nv_dev,
                "Failed to attach content protection properties to connector created from physical index %u",
                nv_connector->physicalIndex);
            goto failed_connector_init;
        }
        /* attach nvidia defined connector properties */
        drm_object_attach_property(&nv_connector->base.base,
                                   nv_dev->nv_hdcp_topology_property,
                                   0);
        drm_object_attach_property(&nv_connector->base.base,
                                   nv_dev->nv_hdcp_level_property,
                                   0);
    }
#endif


#if defined(NV_DRM_CONNECTOR_ATTACH_HDR_OUTPUT_METADATA_PROPERTY_PRESENT)
    if (nv_connector->type == NVKMS_CONNECTOR_TYPE_HDMI) {
#if defined(NV_DRM_MODE_CREATE_DP_COLORSPACE_PROPERTY_HAS_SUPPORTED_COLORSPACES_ARG)
        if (drm_mode_create_hdmi_colorspace_property(
                &nv_connector->base,
                __nv_drm_connector_supported_colorspaces) == 0) {
#else
        if (drm_mode_create_hdmi_colorspace_property(&nv_connector->base) == 0) {
#endif
            drm_connector_attach_colorspace_property(&nv_connector->base);
        }
        drm_connector_attach_hdr_output_metadata_property(&nv_connector->base);

        /* Signalled in the AVI InfoFrame, so HDMI only, like on i915 */
        if (drm_connector_attach_content_type_property(&nv_connector->base) != 0) {
            NV_DRM_DEV_LOG_ERR(
                nv_dev,
                "Failed to attach the content type property to connector created from physical index %u",
                nv_connector->physicalIndex);
        }

        if (nv_dev->nv_connector_hdmi_vsif_metadata_property) {
            drm_object_attach_property(&nv_connector->base.base,
                                       nv_dev->nv_connector_hdmi_vsif_metadata_property,
                                       0);
        }
    } else if (nv_connector->type == NVKMS_CONNECTOR_TYPE_DP) {
#if defined(NV_DRM_MODE_CREATE_DP_COLORSPACE_PROPERTY_HAS_SUPPORTED_COLORSPACES_ARG)
        if (drm_mode_create_dp_colorspace_property(
                &nv_connector->base,
                __nv_drm_connector_supported_colorspaces) == 0) {
#else
        if (drm_mode_create_dp_colorspace_property(&nv_connector->base) == 0) {
#endif
            drm_connector_attach_colorspace_property(&nv_connector->base);
        }
        drm_connector_attach_hdr_output_metadata_property(&nv_connector->base);
    }
#endif /* defined(NV_DRM_CONNECTOR_ATTACH_HDR_OUTPUT_METADATA_PROPERTY_PRESENT) */

#if defined(NV_DRM_CONNECTOR_ATTACH_HDR_OUTPUT_METADATA_PROPERTY_PRESENT)
    /* Applied in connector atomic_check, so only on the kernels that have it */
    if ((nv_connector->type == NVKMS_CONNECTOR_TYPE_DP) ||
        (nv_connector->type == NVKMS_CONNECTOR_TYPE_HDMI) ||
        (nv_connector->type == NVKMS_CONNECTOR_TYPE_DVI_D) ||
        (nv_connector->type == NVKMS_CONNECTOR_TYPE_DVI_I) ||
        (nv_connector->type == NVKMS_CONNECTOR_TYPE_USBC)) {
        drm_object_attach_property(&nv_connector->base.base,
                                   nv_dev->nv_connector_broadcast_rgb_property,
                                   NV_DRM_BROADCAST_RGB_AUTO);

        if (drm_connector_attach_max_bpc_property(&nv_connector->base,
                                                  NV_DRM_MAX_BPC_MIN,
                                                  NV_DRM_MAX_BPC_MAX) != 0) {
            NV_DRM_DEV_LOG_ERR(
                nv_dev,
                "Failed to attach the max bpc property to connector created from physical index %u",
                nv_connector->physicalIndex);
        }
    }

    /*
     * The head scaler can show a mode smaller than the display's native
     * resolution at the native timings. Like on other drivers, no scaling is
     * the default; then the mode is driven as requested.
     */
    if (nv_drm_scaling_mode_module_param &&
        drm_connector_attach_scaling_mode_property(
            &nv_connector->base,
            BIT(DRM_MODE_SCALE_NONE) |
            BIT(DRM_MODE_SCALE_FULLSCREEN) |
            BIT(DRM_MODE_SCALE_CENTER) |
            BIT(DRM_MODE_SCALE_ASPECT)) != 0) {
        NV_DRM_DEV_LOG_ERR(
            nv_dev,
            "Failed to attach the scaling mode property to connector created from physical index %u",
            nv_connector->physicalIndex);
    }

    /*
     * Underscan, for TVs that crop the picture edges: the head scaler
     * shrinks the image by the borders, like on amdgpu.
     */
    if (nv_drm_scaling_mode_module_param) {
        drm_object_attach_property(&nv_connector->base.base,
                                   nv_dev->nv_connector_underscan_property,
                                   NV_DRM_UNDERSCAN_OFF);
        drm_object_attach_property(&nv_connector->base.base,
                                   nv_dev->nv_connector_underscan_hborder_property,
                                   0);
        drm_object_attach_property(&nv_connector->base.base,
                                   nv_dev->nv_connector_underscan_vborder_property,
                                   0);
    }
#endif

    /* Attach dithering mode property */
    if (nv_dev->nv_connector_dithering_mode_property) {
        drm_object_attach_property(&nv_connector->base.base,
                                   nv_dev->nv_connector_dithering_mode_property,
                                   NV_DRM_DITHERING_MODE_AUTO);
    }

    /* Register connector with DRM subsystem */

    nv_drm_connector_set_mst_path(nv_connector);

    ret = drm_connector_register(&nv_connector->base);

    if (ret != 0) {
        NV_DRM_DEV_LOG_ERR(
            nv_dev,
            "Failed to register connector created from physical index %u",
            nv_connector->physicalIndex);
        goto failed_connector_register;
    }

    return &nv_connector->base;

failed_connector_register:
    drm_connector_cleanup(&nv_connector->base);

failed_connector_init:
    nv_drm_free(nv_connector->base.state);

failed_state_alloc:
    nv_drm_free(nv_connector);

failed:
    return ERR_PTR(ret);
}

/*
 * Get connector with given physical index one exists. Otherwise, create and
 * return a new connector.
 */
struct drm_connector*
nv_drm_get_connector(struct drm_device *dev,
                     NvU32 physicalIndex, NvKmsConnectorType type,
                     NvBool internal,
                     char dpAddress[NVKMS_DP_ADDRESS_STRING_LENGTH])
{
    struct drm_connector *connector = NULL;
    struct drm_connector_list_iter conn_iter;
    drm_connector_list_iter_begin(dev, &conn_iter);

    /* Lookup for existing connector with same physical index */
    drm_for_each_connector_iter(connector, &conn_iter) {
        struct nv_drm_connector *nv_connector = to_nv_connector(connector);

        if (nv_connector->physicalIndex == physicalIndex) {
            BUG_ON(nv_connector->type != type ||
                   nv_connector->internal != internal);

            if (strcmp(nv_connector->dpAddress, dpAddress) == 0) {
                goto done;
            }
        }
    }
    connector = NULL;

done:
    drm_connector_list_iter_end(&conn_iter);

    if (!connector) {
        connector = nv_drm_connector_new(dev,
                                         physicalIndex, type, internal,
                                         dpAddress);
    }

    return connector;
}

/*
 * Revoke the permissions on this connector.
 */
bool nv_drm_connector_revoke_permissions(struct drm_device *dev,
                                         struct nv_drm_connector* nv_connector)
{
    struct nv_drm_device *nv_dev = to_nv_device(dev);
    bool ret = true;

    if (nv_connector->modeset_permission_crtc) {
        if (nv_connector->nv_detected_encoder) {
            ret = nvKms->revokePermissions(
                nv_dev->pDevice, nv_connector->modeset_permission_crtc->head,
                nv_connector->nv_detected_encoder->hDisplay);
        }
        nv_connector->modeset_permission_crtc->modeset_permission_filep = NULL;
        nv_connector->modeset_permission_crtc = NULL;
    }
    nv_connector->modeset_permission_filep = NULL;
    return ret;
}

void nv_drm_connector_update_content_protection(struct nv_drm_connector *nv_connector)
{
#ifdef NV_DRM_SUPPORT_CONTENT_PROTECTION_PROPERTY
    struct drm_connector *connector = &nv_connector->base;
    struct drm_connector_state *state = connector->state;
    unsigned int content_protection = state->content_protection;
    unsigned int hdcp_content_type = state->hdcp_content_type;
    bool update_cp = false;
    unsigned int cp_val;

    if ((content_protection == DRM_MODE_CONTENT_PROTECTION_DESIRED) &&
        (hdcp_content_type == DRM_MODE_HDCP_CONTENT_TYPE0)) {
        if ((nv_connector->cp == NVKMS_CONTENT_PROTECTION_HDCP1X_ON) ||
            (nv_connector->cp == NVKMS_CONTENT_PROTECTION_HDCP2X_TYPE0_ON) ||
            (nv_connector->cp == NVKMS_CONTENT_PROTECTION_HDCP2X_TYPE1_ON)) {
            cp_val = DRM_MODE_CONTENT_PROTECTION_ENABLED;
            update_cp = true;
        }
    } else if ((content_protection == DRM_MODE_CONTENT_PROTECTION_DESIRED) &&
             (hdcp_content_type == DRM_MODE_HDCP_CONTENT_TYPE1)) {
        if (nv_connector->cp == NVKMS_CONTENT_PROTECTION_HDCP2X_TYPE1_ON) {
            cp_val = DRM_MODE_CONTENT_PROTECTION_ENABLED;
            update_cp = true;
        }
    } else if (content_protection == DRM_MODE_CONTENT_PROTECTION_ENABLED) {
        if ((nv_connector->cp == NVKMS_CONTENT_PROTECTION_OFF) || 
            (nv_connector->cp == NVKMS_CONTENT_PROTECTION_FAILED)) {
            cp_val = DRM_MODE_CONTENT_PROTECTION_DESIRED;
            update_cp = true;
        }
    }

    if (update_cp) {
        drm_hdcp_update_content_protection(connector, cp_val);
    }
#endif
}

int nv_drm_connector_update_topology_property(struct nv_drm_connector *nv_connector,
                                              const void *topology)
{
#ifdef NV_DRM_SUPPORT_CONTENT_PROTECTION_PROPERTY
    struct drm_connector *connector = &nv_connector->base;
    struct drm_connector_state *state = connector->state;
    struct nv_drm_connector_state *nv_state = to_nv_drm_connector_state(state);
    struct drm_device *dev = connector->dev;
    struct nv_drm_device *nv_dev = to_nv_device(dev);
    const struct drm_property_blob *old_blob = nv_state->hdcp_topology_blob;
    int ret;

    /* The content protection properties are only attached to DP and HDMI */
    if ((nv_connector->type != NVKMS_CONNECTOR_TYPE_DP) &&
        (nv_connector->type != NVKMS_CONNECTOR_TYPE_HDMI)) {
        return 0;
    }

    /*
     * RM reports HDCP disabled/inactive on every DPMS off, which clears an
     * already clear topology.  Don't send a hotplug uevent for that: it makes
     * compositors force-probe every connector.
     */
    if ((old_blob != NULL) ?
        ((old_blob->length == NVKMS_HDCP_TOPOLOGY_SIZE) &&
         (memcmp(old_blob->data, topology, NVKMS_HDCP_TOPOLOGY_SIZE) == 0)) :
        (memchr_inv(topology, 0, NVKMS_HDCP_TOPOLOGY_SIZE) == NULL)) {
        return 0;
    }

    ret = drm_property_replace_global_blob(dev,
                                            &nv_state->hdcp_topology_blob,
                                            NVKMS_HDCP_TOPOLOGY_SIZE,
                                            topology,
                                            &connector->base,
                                            nv_dev->nv_hdcp_topology_property);
    // Generate uevent on cp property when topology is updated
#if defined(NV_DRM_SYSFS_CONNECTOR_PROPERTY_EVENT_PRESENT)
    drm_sysfs_connector_property_event(connector,
        dev->mode_config.content_protection_property);
#elif defined(NV_DRM_SYSFS_CONNECTOR_STATUS_EVENT_PRESENT)
    drm_sysfs_connector_status_event(connector,
        dev->mode_config.content_protection_property);
#endif

    return ret;
#else
    return 0;
#endif
}

#endif
