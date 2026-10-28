/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/*
 * hwmon interface for the GPU sensors.
 *
 * The values come from the RM user shared data (RUSD) page, which GSP fills
 * while someone asks it to poll. Reading the page is lockless and never
 * touches the GPU. Polling is requested on the first read and stopped again
 * NV_HWMON_IDLE_TIMEOUT after the last one. While the GPU is runtime
 * suspended the sensors return -ENODATA instead of waking it.
 *
 * The device is registered only while the adapter is initialized, so a read
 * never races with RM teardown.
 */

#include "os-interface.h"
#include "nv-linux.h"

#if defined(NV_HWMON_DEVICE_REGISTER_WITH_INFO_PRESENT) && IS_REACHABLE(CONFIG_HWMON)

#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/pm_runtime.h>

/* Stop GSP polling after this long without reads */
#define NV_HWMON_IDLE_TIMEOUT       (10 * HZ)

/* The RUSD poll interval is 100-1000 ms; no new sample for this long means GSP stopped */
#define NV_HWMON_STALE_TIMEOUT      (3 * HZ)

/* How long a read waits for the first sample after polling starts */
#define NV_HWMON_FRESH_WAIT_MS      1500
#define NV_HWMON_FRESH_POLL_MS      50

#define NV_HWMON_PROBE_DELAY        (HZ)
#define NV_HWMON_PROBE_TRIES        5
#define NV_HWMON_RETRY_DELAY        (10 * HZ)

static bool nv_hwmon_enable = true;
module_param_named(hwmon, nv_hwmon_enable, bool, 0444);
MODULE_PARM_DESC(hwmon, "Register a hwmon device with the GPU sensors (default: 1).");

static NV_STATUS nv_hwmon_set_polling(nv_linux_state_t *nvl, NvBool enable, NvBool force)
{
    nvidia_stack_t *sp = NULL;
    NV_STATUS status;

    if (nv_kmem_cache_alloc_stack(&sp) != 0)
    {
        return NV_ERR_NO_MEMORY;
    }

    status = rm_hwmon_set_polling(sp, NV_STATE_PTR(nvl), enable, force);

    nv_kmem_cache_free_stack(sp);

    return status;
}

static int nv_hwmon_snapshot(nv_linux_state_t *nvl,
                             nv_hwmon_reading_t *readings,
                             NvU64 *heartbeat)
{
    nvidia_stack_t *sp = NULL;
    unsigned int i;

    if (nv_kmem_cache_alloc_stack(&sp) != 0)
    {
        return -ENOMEM;
    }

    rm_hwmon_read(sp, NV_STATE_PTR(nvl), readings);

    nv_kmem_cache_free_stack(sp);

    /* Any changed timestamp means GSP is still writing the page */
    *heartbeat = 0;
    for (i = 0; i < NV_HWMON_SENSOR_COUNT; i++)
    {
        *heartbeat += readings[i].timestamp;
    }

    return 0;
}

static int nv_hwmon_wait_fresh(nv_linux_state_t *nvl,
                               nv_hwmon_reading_t *readings,
                               NvU64 *heartbeat)
{
    NvU64 old_heartbeat = *heartbeat;
    unsigned int waited;
    int rc;

    for (waited = 0; waited < NV_HWMON_FRESH_WAIT_MS; waited += NV_HWMON_FRESH_POLL_MS)
    {
        msleep(NV_HWMON_FRESH_POLL_MS);

        rc = nv_hwmon_snapshot(nvl, readings, heartbeat);
        if (rc != 0)
        {
            return rc;
        }

        if (*heartbeat != old_heartbeat)
        {
            return 0;
        }
    }

    return -ENODATA;
}

/* Called with nvl->hwmon.lock held */
static int nv_hwmon_get(nv_linux_state_t *nvl, nv_hwmon_sensor_t sensor, long *val)
{
    nv_hwmon_t *hw = &nvl->hwmon;
    nv_hwmon_reading_t readings[NV_HWMON_SENSOR_COUNT];
    NvU64 heartbeat;
    NvBool wait = NV_FALSE;
    int rc;

    if (hw->stopping)
    {
        return -ENODATA;
    }

    /* GSP doesn't update the page while the GPU is suspended; don't wake it */
    if (!pm_runtime_active(nvl->dev))
    {
        return -ENODATA;
    }

    hw->last_read = jiffies;

    /* Taken before polling starts so the first new sample counts as fresh */
    rc = nv_hwmon_snapshot(nvl, readings, &heartbeat);
    if (rc != 0)
    {
        return rc;
    }

    if (!hw->polling)
    {
        if (nv_hwmon_set_polling(nvl, NV_TRUE, NV_FALSE) != NV_OK)
        {
            return -ENODATA;
        }

        hw->polling = NV_TRUE;
        mod_delayed_work(system_freezable_wq, &hw->work, NV_HWMON_IDLE_TIMEOUT);
        wait = NV_TRUE;
    }
    /*
     * Nothing changed for a while although polling is on: GSP lost the
     * request, for example across a power transition. Send it again.
     */
    else if ((heartbeat == hw->heartbeat) &&
             time_after(jiffies, hw->heartbeat_jiffies + NV_HWMON_STALE_TIMEOUT))
    {
        if (hw->stale && time_before(jiffies, hw->retry_jiffies))
        {
            return -ENODATA;
        }

        if (nv_hwmon_set_polling(nvl, NV_TRUE, NV_TRUE) != NV_OK)
        {
            goto stale;
        }

        wait = NV_TRUE;
    }

    if (wait)
    {
        rc = nv_hwmon_wait_fresh(nvl, readings, &heartbeat);
        if (rc == -ENODATA)
        {
            goto stale;
        }
        if (rc != 0)
        {
            return rc;
        }
    }

    if (heartbeat != hw->heartbeat)
    {
        hw->heartbeat = heartbeat;
        hw->heartbeat_jiffies = jiffies;
        hw->stale = NV_FALSE;
    }

    if (readings[sensor].timestamp == 0)
    {
        return -ENODATA;
    }

    *val = readings[sensor].value;

    return 0;

stale:
    /* Don't retry the RPC and the wait on every read */
    hw->stale = NV_TRUE;
    hw->retry_jiffies = jiffies + NV_HWMON_RETRY_DELAY;

    return -ENODATA;
}

static int nv_hwmon_sensor(enum hwmon_sensor_types type, u32 attr, int channel)
{
    switch (type)
    {
        case hwmon_temp:
            if ((attr != hwmon_temp_input) && (attr != hwmon_temp_label))
                break;
            if (channel == 0)
                return NV_HWMON_SENSOR_TEMP_GPU;
            if (channel == 1)
                return NV_HWMON_SENSOR_TEMP_MEMORY;
            break;
        case hwmon_power:
            if (channel != 0)
                break;
            if (attr == hwmon_power_input)
                return NV_HWMON_SENSOR_POWER_INSTANT;
            if (attr == hwmon_power_average)
                return NV_HWMON_SENSOR_POWER_AVERAGE;
            if (attr == hwmon_power_cap)
                return NV_HWMON_SENSOR_POWER_CAP;
            break;
        case hwmon_fan:
            if (attr != hwmon_fan_input)
                break;
            if (channel == 0)
                return NV_HWMON_SENSOR_FAN0;
            if (channel == 1)
                return NV_HWMON_SENSOR_FAN1;
            break;
        default:
            break;
    }

    return -1;
}

static umode_t nv_hwmon_is_visible(const void *data,
                                   enum hwmon_sensor_types type,
                                   u32 attr, int channel)
{
    const nv_linux_state_t *nvl = data;
    int sensor = nv_hwmon_sensor(type, attr, channel);

    if ((sensor < 0) || !(nvl->hwmon.visible & NVBIT(sensor)))
    {
        return 0;
    }

    return 0444;
}

static int nv_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
                         u32 attr, int channel, long *val)
{
    nv_linux_state_t *nvl = dev_get_drvdata(dev);
    int sensor = nv_hwmon_sensor(type, attr, channel);
    int rc;

    if (sensor < 0)
    {
        return -EOPNOTSUPP;
    }

    mutex_lock(&nvl->hwmon.lock);
    rc = nv_hwmon_get(nvl, sensor, val);
    mutex_unlock(&nvl->hwmon.lock);

    return rc;
}

#if defined(NV_HWMON_OPS_READ_STRING_HAS_CONST_STR)
#define NV_HWMON_T_LABEL HWMON_T_LABEL

static int nv_hwmon_read_string(struct device *dev, enum hwmon_sensor_types type,
                                u32 attr, int channel, const char **str)
{
    if ((type != hwmon_temp) || (attr != hwmon_temp_label))
    {
        return -EOPNOTSUPP;
    }

    *str = (channel == 0) ? "gpu" : "mem";

    return 0;
}
#else
#define NV_HWMON_T_LABEL 0
#endif

static const u32 nv_hwmon_temp_config[] = {
    HWMON_T_INPUT | NV_HWMON_T_LABEL,
    HWMON_T_INPUT | NV_HWMON_T_LABEL,
    0
};

static const struct hwmon_channel_info nv_hwmon_temp = {
    .type = hwmon_temp,
    .config = nv_hwmon_temp_config,
};

static const u32 nv_hwmon_power_config[] = {
    HWMON_P_INPUT | HWMON_P_AVERAGE | HWMON_P_CAP,
    0
};

static const struct hwmon_channel_info nv_hwmon_power = {
    .type = hwmon_power,
    .config = nv_hwmon_power_config,
};

static const u32 nv_hwmon_fan_config[] = {
    HWMON_F_INPUT,
    HWMON_F_INPUT,
    0
};

static const struct hwmon_channel_info nv_hwmon_fan = {
    .type = hwmon_fan,
    .config = nv_hwmon_fan_config,
};

static const struct hwmon_channel_info *nv_hwmon_info[] = {
    &nv_hwmon_temp,
    &nv_hwmon_power,
    &nv_hwmon_fan,
    NULL
};

static const struct hwmon_ops nv_hwmon_ops = {
    .is_visible = nv_hwmon_is_visible,
    .read = nv_hwmon_read,
#if defined(NV_HWMON_OPS_READ_STRING_HAS_CONST_STR)
    .read_string = nv_hwmon_read_string,
#endif
};

static const struct hwmon_chip_info nv_hwmon_chip_info = {
    .ops = &nv_hwmon_ops,
    .info = nv_hwmon_info,
};

/* hwmon has no frequency sensor type; amdgpu uses freqN_input/label too */
static ssize_t nv_hwmon_freq_show(struct device *dev,
                                  struct device_attribute *attr,
                                  char *buf)
{
    nv_linux_state_t *nvl = dev_get_drvdata(dev);
    long val;
    int rc;

    mutex_lock(&nvl->hwmon.lock);
    rc = nv_hwmon_get(nvl, to_sensor_dev_attr(attr)->index, &val);
    mutex_unlock(&nvl->hwmon.lock);

    if (rc != 0)
    {
        return rc;
    }

    return scnprintf(buf, PAGE_SIZE, "%ld\n", val);
}

static ssize_t nv_hwmon_freq_label_show(struct device *dev,
                                        struct device_attribute *attr,
                                        char *buf)
{
    int sensor = to_sensor_dev_attr(attr)->index;

    return scnprintf(buf, PAGE_SIZE, "%s\n",
                     (sensor == NV_HWMON_SENSOR_CLOCK_GRAPHICS) ? "gpu" : "mem");
}

static SENSOR_DEVICE_ATTR(freq1_input, 0444, nv_hwmon_freq_show, NULL,
                          NV_HWMON_SENSOR_CLOCK_GRAPHICS);
static SENSOR_DEVICE_ATTR(freq1_label, 0444, nv_hwmon_freq_label_show, NULL,
                          NV_HWMON_SENSOR_CLOCK_GRAPHICS);
static SENSOR_DEVICE_ATTR(freq2_input, 0444, nv_hwmon_freq_show, NULL,
                          NV_HWMON_SENSOR_CLOCK_MEMORY);
static SENSOR_DEVICE_ATTR(freq2_label, 0444, nv_hwmon_freq_label_show, NULL,
                          NV_HWMON_SENSOR_CLOCK_MEMORY);

static struct attribute *nv_hwmon_attrs[] = {
    &sensor_dev_attr_freq1_input.dev_attr.attr,
    &sensor_dev_attr_freq1_label.dev_attr.attr,
    &sensor_dev_attr_freq2_input.dev_attr.attr,
    &sensor_dev_attr_freq2_label.dev_attr.attr,
    NULL
};

static umode_t nv_hwmon_attr_is_visible(struct kobject *kobj,
                                        struct attribute *attr,
                                        int index)
{
    struct device *dev = container_of(kobj, struct device, kobj);
    nv_linux_state_t *nvl = dev_get_drvdata(dev);
    struct sensor_device_attribute *sattr =
        container_of(attr, struct sensor_device_attribute, dev_attr.attr);

    return (nvl->hwmon.visible & NVBIT(sattr->index)) ? attr->mode : 0;
}

static const struct attribute_group nv_hwmon_group = {
    .attrs = nv_hwmon_attrs,
    .is_visible = nv_hwmon_attr_is_visible,
};

static const struct attribute_group *nv_hwmon_groups[] = {
    &nv_hwmon_group,
    NULL
};

/*
 * Poll once to learn which sensors this board has, then register. Called
 * from the work item with nvl->hwmon.lock held.
 */
static void nv_hwmon_probe(nv_linux_state_t *nvl)
{
    nv_hwmon_t *hw = &nvl->hwmon;
    nv_state_t *nv = NV_STATE_PTR(nvl);
    nv_hwmon_reading_t readings[NV_HWMON_SENSOR_COUNT];
    NvU64 heartbeat;
    NV_STATUS status;
    struct device *dev;
    NvU32 visible = 0;
    unsigned int i;

    /* Wait for the GPU to wake up on its own */
    if (!pm_runtime_active(nvl->dev))
    {
        queue_delayed_work(system_freezable_wq, &hw->work, NV_HWMON_RETRY_DELAY);
        return;
    }

    if (!hw->polling)
    {
        status = nv_hwmon_set_polling(nvl, NV_TRUE, NV_FALSE);
        if (status == NV_ERR_NOT_SUPPORTED)
        {
            NV_DEV_PRINTF(NV_DBG_INFO, nv, "hwmon: sensor polling not supported\n");
            return;
        }
        if (status != NV_OK)
        {
            queue_delayed_work(system_freezable_wq, &hw->work, NV_HWMON_RETRY_DELAY);
            return;
        }

        hw->polling = NV_TRUE;
        hw->last_read = jiffies;
        hw->probe_tries = 0;
        queue_delayed_work(system_freezable_wq, &hw->work, NV_HWMON_PROBE_DELAY);
        return;
    }

    if (nv_hwmon_snapshot(nvl, readings, &heartbeat) != 0)
    {
        queue_delayed_work(system_freezable_wq, &hw->work, NV_HWMON_PROBE_DELAY);
        return;
    }

    for (i = 0; i < NV_HWMON_SENSOR_COUNT; i++)
    {
        if (readings[i].timestamp != 0)
        {
            visible |= NVBIT(i);
        }
    }

    /* Boards whose fans are driven by the EC report 0 RPM for every cooler */
    for (i = NV_HWMON_SENSOR_FAN0; i <= NV_HWMON_SENSOR_FAN1; i++)
    {
        if (readings[i].value == 0)
        {
            visible &= ~NVBIT(i);
        }
    }

    if (visible == 0)
    {
        if (++hw->probe_tries < NV_HWMON_PROBE_TRIES)
        {
            queue_delayed_work(system_freezable_wq, &hw->work, NV_HWMON_PROBE_DELAY);
            return;
        }

        NV_DEV_PRINTF(NV_DBG_INFO, nv, "hwmon: no sensor data from GSP\n");
        goto stop_polling;
    }

    hw->visible = visible;
    hw->heartbeat = heartbeat;
    hw->heartbeat_jiffies = jiffies;

    dev = hwmon_device_register_with_info(nvl->dev, "nvidia", nvl,
                                          &nv_hwmon_chip_info,
                                          nv_hwmon_groups);
    if (IS_ERR(dev))
    {
        NV_DEV_PRINTF(NV_DBG_ERRORS, nv, "hwmon: registration failed: %ld\n",
                      PTR_ERR(dev));
        goto stop_polling;
    }

    hw->dev = dev;

    /* Stop the probe's polling unless someone reads */
    queue_delayed_work(system_freezable_wq, &hw->work, NV_HWMON_IDLE_TIMEOUT);
    return;

stop_polling:
    if (nv_hwmon_set_polling(nvl, NV_FALSE, NV_FALSE) == NV_OK)
    {
        hw->polling = NV_FALSE;
    }
}

static void nv_hwmon_work(struct work_struct *work)
{
    nv_hwmon_t *hw = container_of(to_delayed_work(work), nv_hwmon_t, work);
    nv_linux_state_t *nvl = container_of(hw, nv_linux_state_t, hwmon);

    mutex_lock(&hw->lock);

    if (hw->stopping)
    {
        goto done;
    }

    if (hw->dev == NULL)
    {
        nv_hwmon_probe(nvl);
        goto done;
    }

    if (!hw->polling)
    {
        goto done;
    }

    if (time_before(jiffies, hw->last_read + NV_HWMON_IDLE_TIMEOUT))
    {
        queue_delayed_work(system_freezable_wq, &hw->work,
                           hw->last_read + NV_HWMON_IDLE_TIMEOUT - jiffies);
        goto done;
    }

    /* Don't power up the GPU just to stop polling; retry once it is awake */
    if (!pm_runtime_active(nvl->dev) ||
        (nv_hwmon_set_polling(nvl, NV_FALSE, NV_FALSE) != NV_OK))
    {
        queue_delayed_work(system_freezable_wq, &hw->work, NV_HWMON_RETRY_DELAY);
        goto done;
    }

    hw->polling = NV_FALSE;

done:
    mutex_unlock(&hw->lock);
}

void nv_hwmon_init(nv_linux_state_t *nvl)
{
    nv_hwmon_t *hw = &nvl->hwmon;

    hw->dev = NULL;
    hw->stopping = NV_FALSE;
    hw->polling = NV_FALSE;
    hw->probe_tries = 0;
    hw->visible = 0;
    hw->last_read = jiffies;
    hw->heartbeat = 0;
    hw->heartbeat_jiffies = jiffies;
    hw->stale = NV_FALSE;
    hw->retry_jiffies = jiffies;
    mutex_init(&hw->lock);
    INIT_DELAYED_WORK(&hw->work, nv_hwmon_work);
}

/* Called once the adapter is initialized */
void nv_hwmon_start(nv_linux_state_t *nvl)
{
    nv_hwmon_t *hw = &nvl->hwmon;

    if (!nv_hwmon_enable || !dev_is_pci(nvl->dev))
    {
        return;
    }

    mutex_lock(&hw->lock);

    hw->stopping = NV_FALSE;
    hw->probe_tries = 0;
    hw->stale = NV_FALSE;

    if (hw->dev == NULL)
    {
        queue_delayed_work(system_freezable_wq, &hw->work, 0);
    }

    mutex_unlock(&hw->lock);
}

/* Called before the adapter is shut down; the GPU must still be accessible */
void nv_hwmon_stop(nv_linux_state_t *nvl, nvidia_stack_t *sp)
{
    nv_hwmon_t *hw = &nvl->hwmon;
    struct device *dev;

    if (!dev_is_pci(nvl->dev))
    {
        return;
    }

    mutex_lock(&hw->lock);
    hw->stopping = NV_TRUE;
    dev = hw->dev;
    hw->dev = NULL;
    mutex_unlock(&hw->lock);

    cancel_delayed_work_sync(&hw->work);

    /* Waits for the readers still in a sysfs callback */
    if (dev != NULL)
    {
        hwmon_device_unregister(dev);
    }

    mutex_lock(&hw->lock);
    if (hw->polling)
    {
        (void)rm_hwmon_set_polling(sp, NV_STATE_PTR(nvl), NV_FALSE, NV_FALSE);
        hw->polling = NV_FALSE;
    }
    mutex_unlock(&hw->lock);
}

#else

void nv_hwmon_init(nv_linux_state_t *nvl)
{
}

void nv_hwmon_start(nv_linux_state_t *nvl)
{
}

void nv_hwmon_stop(nv_linux_state_t *nvl, nvidia_stack_t *sp)
{
}

#endif
