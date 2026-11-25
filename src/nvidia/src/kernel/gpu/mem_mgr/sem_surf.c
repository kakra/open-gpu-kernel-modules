/*
 * SPDX-FileCopyrightText: Copyright (c) 2023-2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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

#include "gpu/mem_mgr/sem_surf.h"
#include "os/os.h" // NV_MEMORY_NONCONTIGUOUS, osEventNotification
#include "gpu/device/device.h"
#include "gpu_mgr/gpu_mgr.h"
#include "gpu/mem_mgr/mem_mgr.h"
#include "gpu/mem_mgr/mem_desc.h"
#include "gpu/fifo/kernel_channel.h"
#include "gpu/gpu.h"
#include "core/locks.h"
#include "rmapi/client.h"
#include "rmapi/rs_utils.h"
#include "platform/sli/sli.h"

#include "class/cl0080.h"
#include "class/cl2080.h"
#include "class/cl0005.h"
#include "class/cl003e.h" // NV01_MEMORY_SYSTEM
#include "class/cl0040.h" // NV01_MEMORY_LOCAL_USER
#include "nv-fix-hits.h"

//
// Non-stall interrupt dispatch
//
// Each (GPU, internal client, notify index) has one SEM_DISPATCHER owning a
// single NV01_EVENT_KERNEL_CALLBACK_EX event that is shared by every
// semaphore surface bound to a channel there. Previously each surface
// allocated its own event, so every non-stall interrupt ran one callback per
// live surface in the ISR, even for surfaces without waiters, and creating or
// destroying a surface paid for an event alloc/free (a GSP RPC) plus linear
// searches of the engine and subdevice event lists.
//
// The dispatcher only visits armed bindings. A binding is armed whenever its
// surface has listeners: the waiter path arms before it lowers the monitored
// fence, so an interrupt caused by that fence finds the binding, and a
// binding is disarmed under the surface spinlock once listenerMap is empty.
// Invariant: listenerMap non-empty => every binding in notifierMap is armed.
//
// Lock order: SEM_SHARED_DATA::pSpinlock -> SEM_DISPATCHER::pLock. The
// dispatch callback drops pLock before taking any surface spinlock.
//
// Dispatchers are looked up, created and destroyed with the GPU lock of
// their GPU held, which the RM event alloc/free they do requires anyway.
//
MAKE_INTRUSIVE_LIST(SEM_ARMED_LIST, SEM_NOTIFIER_NODE, armedEntry);
MAKE_INTRUSIVE_LIST(SEM_DISPATCH_PENDING_LIST, SEM_NOTIFIER_NODE, pendingEntry);

typedef struct SEM_DISPATCHER
{
    struct SEM_DISPATCHER *pNext;
    NvU32 gpuInstance;
    NvHandle hClient;
    NvHandle hEvent;
    NvU32 notifyIndex;

    /* Number of bindings using this dispatcher. GPU lock. */
    NvU32 refCount;

    NVOS10_EVENT_KERNEL_CALLBACK_EX callback;

    /* Protects armedList, pendingList, activeDispatchers, node bArmed/bPending */
    PORT_SPINLOCK *pLock;
    SEM_ARMED_LIST armedList;

    /*
     * Armed nodes queued by running dispatch callbacks, walked outside of
     * pLock. Nodes can't leave it, and can't be freed, while
     * activeDispatchers is non-zero; the last dispatcher out empties it.
     */
    SEM_DISPATCH_PENDING_LIST pendingList;
    volatile NvU32 activeDispatchers;
} SEM_DISPATCHER;

/* Per-GPU dispatcher lists, protected by that GPU's lock */
static SEM_DISPATCHER *semsurfDispatchers[NV_MAX_DEVICES];

static void _semsurfEventCallback(SEM_SHARED_DATA *pShared, SEM_NOTIFIER_NODE *pNode);

/* Must be called with pNode->pShared->pSpinlock held */
static void
_semsurfNodeArm
(
    SEM_NOTIFIER_NODE *pNode
)
{
    SEM_DISPATCHER *pDisp = pNode->pDispatcher;

    portSyncSpinlockAcquire(pDisp->pLock);
    if (!pNode->bArmed)
    {
        pNode->bArmed = NV_TRUE;
        listAppendExisting(&pDisp->armedList, pNode);
    }
    portSyncSpinlockRelease(pDisp->pLock);
}

/* Must be called with pNode->pShared->pSpinlock held, or after the node left notifierMap */
static void
_semsurfNodeDisarm
(
    SEM_NOTIFIER_NODE *pNode
)
{
    SEM_DISPATCHER *pDisp = pNode->pDispatcher;

    portSyncSpinlockAcquire(pDisp->pLock);
    if (pNode->bArmed)
    {
        pNode->bArmed = NV_FALSE;
        listRemove(&pDisp->armedList, pNode);
    }
    portSyncSpinlockRelease(pDisp->pLock);
}

/* Must be called with pShared->pSpinlock held */
static void
_semsurfArmAll
(
    SEM_SHARED_DATA *pShared
)
{
    SEM_NOTIFIERIter it = mapIterAll(&pShared->notifierMap);

    while (mapIterNext(&it))
        _semsurfNodeArm(it.pValue);
}

/* Must be called with pShared->pSpinlock held */
static void
_semsurfDisarmAllIfIdle
(
    SEM_SHARED_DATA *pShared
)
{
    SEM_NOTIFIERIter it;

    if (mapCount(&pShared->listenerMap) != 0)
        return;

    it = mapIterAll(&pShared->notifierMap);
    while (mapIterNext(&it))
        _semsurfNodeDisarm(it.pValue);
}

//
// Non-stall interrupt callback, called from the lockless ISR on Linux.
// Same pending-list scheme as _gpuEngineEventNotificationListNotify().
//
static void
_semsurfDispatchCallback
(
    void        *pArg,
    void        *pData,
    NvHandle     hEvent,
    NvU32        data,
    NvU32        status
)
{
    SEM_DISPATCHER *pDisp = pArg;
    SEM_ARMED_LISTIter it;
    SEM_NOTIFIER_NODE *pIter, *pTail;

    portSyncSpinlockAcquire(pDisp->pLock);

    // Nothing has waiters; anything still pending belongs to another callback
    if (listCount(&pDisp->armedList) == 0)
    {
        portSyncSpinlockRelease(pDisp->pLock);
        return;
    }

    pDisp->activeDispatchers++;

    it = listIterAll(&pDisp->armedList);
    while (listIterNext(&it))
    {
        if (!it.pValue->bPending)
        {
            it.pValue->bPending = NV_TRUE;
            listAppendExisting(&pDisp->pendingList, it.pValue);
        }
    }

    //
    // Another dispatcher may append to the pending list once pLock is
    // dropped, so walk it by hand up to the tail seen here, like
    // _gpuEngineEventNotificationListNotify() does.
    //
    pIter = listHead(&pDisp->pendingList);
    pTail = listTail(&pDisp->pendingList);

    portSyncSpinlockRelease(pDisp->pLock);

    while (pIter != NULL)
    {
        _semsurfEventCallback(pIter->pShared, pIter);

        if (pIter == pTail)
            break;

        pIter = listNext(&pDisp->pendingList, pIter);
    }

    portSyncSpinlockAcquire(pDisp->pLock);
    if (--pDisp->activeDispatchers == 0)
    {
        SEM_NOTIFIER_NODE *pNext;

        for (pIter = listHead(&pDisp->pendingList); pIter != NULL; pIter = pNext)
        {
            pNext = listNext(&pDisp->pendingList, pIter);
            pIter->bPending = NV_FALSE;
            listRemove(&pDisp->pendingList, pIter);
        }
    }
    portSyncSpinlockRelease(pDisp->pLock);
}

//
// Take pDisp->pLock once no dispatch callback is running, so that the
// pending list is empty and no callback holds a pointer to any node.
// Thread context only, see _gpuEngineEventNotificationListLockPreemptible().
//
static void
_semsurfDispatcherLockIdle
(
    SEM_DISPATCHER *pDisp
)
{
    do
    {
        portSyncSpinlockAcquire(pDisp->pLock);

        if (pDisp->activeDispatchers == 0)
            return;

        portSyncSpinlockRelease(pDisp->pLock);

        while (pDisp->activeDispatchers > 0)
            osSpinLoop();
    } while (NV_TRUE);
}

/*
 * Take the node out of dispatch. The node must already be out of its
 * surface's notifierMap, so nothing can arm it again. On return no dispatch
 * callback references it.
 */
static void
_semsurfNodeDetach
(
    SEM_NOTIFIER_NODE *pNode
)
{
    SEM_DISPATCHER *pDisp = pNode->pDispatcher;

    _semsurfDispatcherLockIdle(pDisp);
    if (pNode->bArmed)
    {
        pNode->bArmed = NV_FALSE;
        listRemove(&pDisp->armedList, pNode);
    }
    NV_ASSERT(!pNode->bPending);
    portSyncSpinlockRelease(pDisp->pLock);
}

static void
_semsurfDispatcherFree
(
    SEM_DISPATCHER *pDisp
)
{
    RM_API *pRmApi = rmapiGetInterface(RMAPI_GPU_LOCK_INTERNAL);

    //
    // Freeing the event waits for any running engine notification pass, so
    // the callback can't run on pDisp after this.
    //
    if (pDisp->hEvent != NV01_NULL_OBJECT)
        pRmApi->Free(pRmApi, pDisp->hClient, pDisp->hEvent);

    NV_ASSERT(listCount(&pDisp->armedList) == 0);
    NV_ASSERT(listCount(&pDisp->pendingList) == 0);

    if (pDisp->pLock != NULL)
        portSyncSpinlockDestroy(pDisp->pLock);

    portMemFree(pDisp);
}

static NV_STATUS
_semsurfDispatcherGet
(
    OBJGPU *pGpu,
    KernelChannel *pKernelChannel,
    NvU32 notifyIndex,
    SEM_DISPATCHER **ppDisp
)
{
    NV0005_ALLOC_PARAMETERS nv0005AllocParams;
    RM_API *pRmApi = rmapiGetInterface(RMAPI_GPU_LOCK_INTERNAL);
    const NvU32 gpuInst = gpuGetInstance(pGpu);
    SEM_DISPATCHER *pDisp;
    NvHandle hClient = NV01_NULL_OBJECT;
    NvHandle hSubdevice = NV01_NULL_OBJECT;
    NV_STATUS status;

    NV_ASSERT_OR_RETURN(gpuInst < NV_ARRAY_ELEMENTS(semsurfDispatchers),
                        NV_ERR_INVALID_STATE);
    NV_ASSERT(rmDeviceGpuLockIsOwner(gpuInst));

    NV_ASSERT_OK_OR_RETURN(memmgrGetInternalClientHandles(pGpu,
                                                          GPU_GET_MEMORY_MANAGER(pGpu),
                                                          GPU_RES_GET_DEVICE(pKernelChannel),
                                                          &hClient,
                                                          NULL,
                                                          &hSubdevice));

    for (pDisp = semsurfDispatchers[gpuInst]; pDisp != NULL; pDisp = pDisp->pNext)
    {
        if ((pDisp->hClient == hClient) && (pDisp->notifyIndex == notifyIndex))
        {
            NV_ASSERT_OR_RETURN(pDisp->refCount < NV_U32_MAX, NV_ERR_INVALID_STATE);
            pDisp->refCount++;
            *ppDisp = pDisp;
            return NV_OK;
        }
    }

    pDisp = portMemAllocNonPaged(sizeof(*pDisp));
    NV_ASSERT_OR_RETURN(pDisp != NULL, NV_ERR_NO_MEMORY);
    portMemSet(pDisp, 0, sizeof(*pDisp));

    pDisp->gpuInstance = gpuInst;
    pDisp->hClient = hClient;
    pDisp->notifyIndex = notifyIndex;
    pDisp->refCount = 1;
    listInitIntrusive(&pDisp->armedList);
    listInitIntrusive(&pDisp->pendingList);

    pDisp->pLock = portSyncSpinlockCreate(portMemAllocatorGetGlobalNonPaged());
    if (pDisp->pLock == NULL)
    {
        status = NV_ERR_NO_MEMORY;
        goto failed;
    }

    pDisp->callback.func = _semsurfDispatchCallback;
    pDisp->callback.arg = pDisp;

    portMemSet(&nv0005AllocParams, 0, sizeof(nv0005AllocParams));
    nv0005AllocParams.hParentClient = hClient;
    nv0005AllocParams.hClass        = NV01_EVENT_KERNEL_CALLBACK_EX;
    nv0005AllocParams.data          = NV_PTR_TO_NvP64(&pDisp->callback);
    nv0005AllocParams.notifyIndex = notifyIndex |
        NV01_EVENT_NONSTALL_INTR |
        NV01_EVENT_WITHOUT_EVENT_DATA |
        NV01_EVENT_SUBDEVICE_SPECIFIC |
        DRF_NUM(0005, _NOTIFY_INDEX, _SUBDEVICE,
                gpumgrGetSubDeviceInstanceFromGpu(pGpu));

    status = pRmApi->Alloc(pRmApi,
                           hClient,
                           hSubdevice,
                           &pDisp->hEvent,
                           NV01_EVENT_KERNEL_CALLBACK_EX,
                           &nv0005AllocParams,
                           sizeof(nv0005AllocParams));
    if (status != NV_OK)
    {
        NV_ASSERT_OK_FAILED("semaphore surface event alloc", status);
        pDisp->hEvent = NV01_NULL_OBJECT;
        goto failed;
    }

    pDisp->pNext = semsurfDispatchers[gpuInst];
    semsurfDispatchers[gpuInst] = pDisp;

    *ppDisp = pDisp;
    return NV_OK;

failed:
    _semsurfDispatcherFree(pDisp);
    return status;
}

static void
_semsurfDispatcherPut
(
    SEM_DISPATCHER *pDisp
)
{
    SEM_DISPATCHER **ppIter;

    NV_ASSERT(rmDeviceGpuLockIsOwner(pDisp->gpuInstance));
    NV_ASSERT_OR_RETURN_VOID(pDisp->refCount > 0);

    if (--pDisp->refCount != 0)
        return;

    for (ppIter = &semsurfDispatchers[pDisp->gpuInstance];
         *ppIter != NULL;
         ppIter = &(*ppIter)->pNext)
    {
        if (*ppIter == pDisp)
        {
            *ppIter = pDisp->pNext;
            break;
        }
    }

    _semsurfDispatcherFree(pDisp);
}

/*
 * Drop a binding that is no longer in its surface's notifierMap.
 * GPU lock of the binding's GPU held.
 */
static void
_semsurfNodeDestroy
(
    SEM_NOTIFIER_NODE *pNode
)
{
    _semsurfNodeDetach(pNode);
    _semsurfDispatcherPut(pNode->pDispatcher);
    portMemFree(pNode);
}

static NvU64
_semsurfGetValue
(
    SEM_SHARED_DATA *pShared,
    NvU64 index
)
{
    volatile NvU8 *pSem          = pShared->pSem;
    volatile NvU8 *pMaxSubmitted = pShared->pMaxSubmitted;
    volatile NvU8 *pSemBase      = pSem + index * pShared->layout.size;

    portAtomicMemoryFenceFull();

    if (pShared->bIs64Bit)
    {
        volatile NvU64 *pSemVal = (volatile NvU64 *)pSemBase;

        NV_PRINTF(LEVEL_INFO, " Read semaphore surface value as 64-bit native\n");

        return *pSemVal;
    }
    else
    {
        const volatile NvU32 *pSemVal           = (volatile NvU32 *)pSemBase;
        volatile NvU8        *pMaxSubmittedBase = pMaxSubmitted + index * pShared->layout.size;
        PORT_ATOMIC NvU64 *pMaxSubmitted        = (PORT_ATOMIC NvU64 *)(pMaxSubmittedBase + pShared->layout.maxSubmittedSemaphoreValueOffset);

        // The ordering below is critical. See NvTimeSemFermiGetPayload() for full comment.
        // TODO Share this code?
        NvU64 semVal = *pSemVal;

        portAtomicMemoryFenceLoad();

        NvU64 maxSubmitted = portAtomicExAddU64(pMaxSubmitted, 0ULL);

        NV_PRINTF(LEVEL_INFO, " Read maxSubmitted %" NvU64_fmtu " and 32-bit semVal %"
                  NvU64_fmtu " from semaphore index %" NvU64_fmtu "\n",
                  maxSubmitted, semVal, index);

        // The value is monotonically increasing, and the max outstanding
        // wait and the value can differ by no more than 2^31-1. Hence...
        if ((maxSubmitted & 0xFFFFFFFFull) < semVal)
            maxSubmitted -= 0x100000000ull;

        return semVal | (maxSubmitted & 0xffffffff00000000ull);
    }
}

static void
_semsurfSetValue
(
    SEM_SHARED_DATA *pShared,
    NvU64 index,
    NvU64 newValue
)
{
    volatile NvU8 *pSem          = pShared->pSem;
    volatile NvU8 *pMaxSubmitted = pShared->pMaxSubmitted;
    volatile NvU8 *pSemBase      = pSem + index * pShared->layout.size;

    if (pShared->bIs64Bit)
    {
        volatile NvU64 *pSemVal = (volatile NvU64 *)pSemBase;

        portAtomicMemoryFenceFull();

        *pSemVal = newValue;

        NV_PRINTF(LEVEL_INFO, " Updated semaphore surface value as 64-bit "
                  "native to %" NvU64_fmtu "\n", newValue);
    }
    else
    {
        volatile NvU32 *pSemVal           = (volatile NvU32 *)pSemBase;
        volatile NvU8  *pMaxSubmittedBase = pMaxSubmitted + index * pShared->layout.size;
        PORT_ATOMIC NvU64 *pMaxSubmitted = (PORT_ATOMIC NvU64 *)(pMaxSubmittedBase + pShared->layout.maxSubmittedSemaphoreValueOffset);
        NvU64 oldMax, origMax;

        portAtomicMemoryFenceFull();
        origMax = oldMax = portAtomicExAddU64(pMaxSubmitted, 0ULL);

        // First save the actual value to the max submitted slot using
        // an atomic max operation
        while (oldMax < newValue)
        {
            NvBool exchanged;

            // XXX Does CompareAndSwap imply any barriers? The comments in
            // nvport/atomic.h imply it does, but that this shouldn't be relied
            // upon, so include another barrier here.
            portAtomicMemoryFenceFull();
            exchanged = portAtomicExCompareAndSwapU64(pMaxSubmitted,
                                                      newValue,
                                                      oldMax);

            if (exchanged)
                break; // The value was set. Exit.

            // The "current" value changed. Update it.
            oldMax = portAtomicExAddU64(pMaxSubmitted, 0);
        }

        portAtomicMemoryFenceStore();

        // Now write the low bits to the GPU-accessible semaphore value.
        *pSemVal = NvU64_LO32(newValue);

        NV_PRINTF(LEVEL_INFO, " Updated maxSubmitted from %" NvU64_fmtu " to %"
                  NvU64_fmtu " and 32-bit semVal %u"
                  " at semaphore index %" NvU64_fmtu "\n",
                  origMax, newValue, NvU64_LO32(newValue), index);
        (void) origMax;
    }
}

static NvBool
_semsurfNotifyCompleted
(
    SEM_SHARED_DATA *pShared,
    SEM_PENDING_NOTIFICATIONS *notifications
)
{
    OBJGPU *pGpu = pShared->pSemaphoreMem->pGpu;
    SEM_PENDING_NOTIFICATIONSIter pendIter;
    SEM_VALUE_LISTENERS_NODE *pVNode;
    NV_STATUS rmStatus;
    NvBool valuesChanged = NV_FALSE;
    EVENTNOTIFICATION **ppListeners;

    for (pendIter = listIterAll(notifications);
         listIterNext(&pendIter);
         pendIter = listIterAll(notifications))
    {
        pVNode = pendIter.pValue;

        rmStatus = osEventNotification(pGpu,
                                       pVNode->pListeners,
                                       NV_SEMAPHORE_SURFACE_WAIT_VALUE,
                                       NULL,
                                       0);

        NV_PRINTF(LEVEL_INFO,
                  "SemMem(0x%08x, 0x%08x): Delivered OS events for value %"
                  NvU64_fmtu " at idx %" NvU64_fmtu ". Status: %s (0x%08x)\n",
                  pShared->hClient, pShared->hSemaphoreMem,
                  pVNode->value, pVNode->index,
                  nvstatusToString(rmStatus),
                  rmStatus);
        (void) rmStatus;

        /*
         * Auto-remove the event listeners for this value now that they
         * have been notified.  It makes little sense to keep notifying
         * them after this.
         */
        ppListeners = &pendIter.pValue->pListeners;
        while (*ppListeners)
        {
            unregisterEventNotificationWithData(ppListeners,
                                                (*ppListeners)->hEventClient,
                                                NV01_NULL_OBJECT, /* hNotifier/subdevice */
                                                (*ppListeners)->hEvent,
                                                NV_TRUE, /* match data/notificationHandle */
                                                (*ppListeners)->Data);
        }

        if (pVNode->newValue != 0) {
            /*
             * It is safe to do this operation outside of the spinlock because
             * it is the RM client's responsibility to ensure the semaphore
             * surface value is monotonically incrementing by explicitly
             * ordering all writes to it, including these. This does imply the
             * need to manually fence memory operations touching the semaphore
             * value though.
             */
            _semsurfSetValue(pShared, pVNode->index, pVNode->newValue);

            NV_PRINTF(LEVEL_INFO,
                      "SemMem(0x%08x, 0x%08x): Value updated by waiter "
                      " to %" NvU64_fmtu " at idx %" NvU64_fmtu "\n",
                      pShared->hClient, pShared->hSemaphoreMem, pVNode->newValue, pVNode->index);

            valuesChanged = NV_TRUE;
        }

        listRemove(notifications, pendIter.pValue);
        portMemFree(pendIter.pValue);
    }

    return valuesChanged;
}

static void
_semsurfSetMonitoredValue
(
    SEM_SHARED_DATA *pShared,
    NvU64 index,
    NvU64 value
)
{
    if (!pShared->bHasMonitoredFence)
        return;

    /*
     * This function must be called with the spinlock held to avoid a race
     * condition where two threads disagree on the current minimum wait value
     * for a given slot.
     */
    volatile NvU64 *pMonitoredFence =
        (volatile NvU64 *)(pShared->pSem +
                           pShared->layout.size * index +
                           pShared->layout.monitoredFenceThresholdOffset);

    NV_PRINTF(LEVEL_INFO,
              "SemMem(0x%08x, 0x%08x): "
              "Setting monitored fence value at index %" NvU64_fmtu
              " to %" NvU64_fmtu "\n",
              pShared->hClient, pShared->hSemaphoreMem,
              index, value);

    // Don't care if this races with loads, but ensure it happens in order
    // with prior stores.
    portAtomicMemoryFenceStore();
    *pMonitoredFence = value;
}

/*
 * Process the listeners of an armed surface binding, from the dispatch
 * callback. Disarms the binding if the surface has no listeners left.
 */
static void
_semsurfEventCallback
(
    SEM_SHARED_DATA   *pShared,
    SEM_NOTIFIER_NODE *pNode
)
{
    SEM_INDEX_LISTENERSIter ilIter;
    SEM_VALUE_LISTENERSIter vlIter;
    SEM_PENDING_NOTIFICATIONS notifications;
    NvU64 index;
    NvU64 semValue;
    NvU64 minWaitValue;
    NvBool removedIndex = NV_FALSE;
    NvBool valuesChanged = NV_TRUE;

    NV_PRINTF(LEVEL_INFO, "SemMem(0x%08x, 0x%08x): Got a callback\n", pShared->hClient, pShared->hSemaphoreMem);

    while (valuesChanged)
    {
        listInitIntrusive(&notifications);

        NV_PRINTF(LEVEL_INFO, "SemMem(0x%08x, 0x%08x): Entering spinlock\n",
                  pShared->hClient,
                  pShared->hSemaphoreMem);
        portSyncSpinlockAcquire(pShared->pSpinlock);

        for (ilIter = mapIterAll(&pShared->listenerMap);
             mapIterNext(&ilIter);
             removedIndex ? ilIter = mapIterAll(&pShared->listenerMap) :
             ilIter)
        {
            removedIndex = NV_FALSE;
            minWaitValue = NV_U64_MAX;

            /*
             * TODO Only notify if vlIter.pValue->value <= the semaphore current
             * value at mapKey(&pShared->listenerMap, ilIter->pValue)
             */
            index = mapKey(&pShared->listenerMap, ilIter.pValue);
            semValue = _semsurfGetValue(pShared, index);

            for (vlIter = listIterAll(&ilIter.pValue->listeners);
                 listIterNext(&vlIter);
                 vlIter = listIterAll(&ilIter.pValue->listeners))
            {
                NV_PRINTF(LEVEL_SILENT,
                          "  Checking index %" NvU64_fmtu " value waiter %"
                          NvU64_fmtu " against semaphore value %" NvU64_fmtu "\n",
                          index, vlIter.pValue->value, semValue);

                if (semValue >= vlIter.pValue->value)
                {
                    listInsertExisting(&notifications, NULL, vlIter.pValue);
                    listRemove(&ilIter.pValue->listeners, vlIter.pValue);
                }
                else
                {
                    /* No other values at this index should be signaled yet. */
                    minWaitValue = vlIter.pValue->value;
                    break;
                }
            }

            if (listCount(&ilIter.pValue->listeners) == 0)
            {
                NV_ASSERT(minWaitValue == NV_U64_MAX);
                mapRemove(&pShared->listenerMap, ilIter.pValue);
                portMemFree(ilIter.pValue);
                removedIndex = NV_TRUE;
            }
            else
            {
                removedIndex = NV_FALSE;
            }

            _semsurfSetMonitoredValue(pShared, index, minWaitValue);
        }

        //
        // Must be decided under the spinlock: a waiter registered after this
        // finds the node disarmed and arms it again before lowering the
        // monitored fence.
        //
        if (mapCount(&pShared->listenerMap) == 0)
            _semsurfNodeDisarm(pNode);

        portSyncSpinlockRelease(pShared->pSpinlock);
        NV_PRINTF(LEVEL_INFO, "SemMem(0x%08x, 0x%08x): Exited spinlock\n",
                  pShared->hClient,
                  pShared->hSemaphoreMem);

        // Send notifications outside of spinlock. They have already been removed
        // from the object-wide lists, so their existance is private to this
        // instance of this function now. Hence, no locking is required for this
        // step.
        valuesChanged = _semsurfNotifyCompleted(pShared, &notifications);
    }
}

static void
_semsurfFreeMemory
(
    SEM_SHARED_DATA *pShared
)
{
    RM_API *pRmApi = rmapiGetInterface(RMAPI_GPU_LOCK_INTERNAL);

    if (pShared->hClient != NV01_NULL_OBJECT)
    {
        if ((pShared->hMaxSubmittedMem != NV01_NULL_OBJECT) &&
            (pShared->hMaxSubmittedMem != pShared->hSemaphoreMem))
            pRmApi->Free(pRmApi,
                         pShared->hClient,
                         pShared->hMaxSubmittedMem);

        if (pShared->hSemaphoreMem != NV01_NULL_OBJECT)
            pRmApi->Free(pRmApi,
                         pShared->hClient,
                         pShared->hSemaphoreMem);
    }

    pShared->hSemaphoreMem    = NV01_NULL_OBJECT;
    pShared->hMaxSubmittedMem = NV01_NULL_OBJECT;
}

static NV_STATUS
_semsurfDupMemory
(
    SemaphoreSurface *pSemSurf,
    NV_SEMAPHORE_SURFACE_ALLOC_PARAMETERS  *pAllocParams
)
{
    SEM_SHARED_DATA *pShared = pSemSurf->pShared;
    RM_API *pRmApi = rmapiGetInterface(RMAPI_GPU_LOCK_INTERNAL);

    NV_CHECK_OK_OR_RETURN(LEVEL_ERROR,
        pRmApi->DupObject(pRmApi,
                          pShared->hClient,
                          pShared->hDevice,
                          &pShared->hSemaphoreMem,
                          RES_GET_CLIENT_HANDLE(pSemSurf),
                          pAllocParams->hSemaphoreMem,
                          NV04_DUP_HANDLE_FLAGS_NONE));

    if (pAllocParams->hMaxSubmittedMem != NV01_NULL_OBJECT)
    {
        NV_CHECK_OR_RETURN(LEVEL_NOTICE,
                           !pSemSurf->pShared->bIs64Bit,
                           NV_ERR_INVALID_PARAMETER);

        if (pAllocParams->hMaxSubmittedMem != pAllocParams->hSemaphoreMem)
        {
            NV_CHECK_OK_OR_RETURN(LEVEL_ERROR,
                pRmApi->DupObject(pRmApi,
                                  pShared->hClient,
                                  pShared->hDevice,
                                  &pShared->hMaxSubmittedMem,
                                  RES_GET_CLIENT_HANDLE(pSemSurf),
                                  pAllocParams->hMaxSubmittedMem,
                                  NV04_DUP_HANDLE_FLAGS_NONE));
        }
        else
        {
            pSemSurf->pShared->hMaxSubmittedMem =
                pSemSurf->pShared->hSemaphoreMem;
        }
    }
    else
    {
        pSemSurf->pShared->hMaxSubmittedMem = NV01_NULL_OBJECT;
    }

    return NV_OK;
}

NV_STATUS
_semsurfRemoveNotifyBinding
(
    SemaphoreSurface *pSemSurf,
    NvU32 gpuIdx,
    NvU32 notifyIndex
)
{
    const NvU64 mapKey = ((NvU64)gpuIdx << 32) | notifyIndex;
    SEM_NOTIFIER_NODE *pNotNode = mapFind(&pSemSurf->pShared->notifierMap, mapKey);

    NV_ASSERT_OR_RETURN(pNotNode != NULL, NV_ERR_INVALID_STATE);
    NV_ASSERT_OR_RETURN(pNotNode->nUsers >= 1, NV_ERR_INVALID_STATE);
    pNotNode->nUsers--;

    if (pNotNode->nUsers != 0)
    {
        return NV_OK;
    }

    portSyncSpinlockAcquire(pSemSurf->pShared->pSpinlock);
    mapRemove(&pSemSurf->pShared->notifierMap, pNotNode);
    portSyncSpinlockRelease(pSemSurf->pShared->pSpinlock);

    _semsurfNodeDestroy(pNotNode);

    return NV_OK;
}

static NV_STATUS
_semsurfUnbindChannel
(
    SemaphoreSurface *pSemSurf,
    SEM_CHANNEL_NODE *pChannelNode
)
{
    NV_STATUS rmStatus = NV_OK;
    NvU32 i;

    for (i = 0; i < pChannelNode->numNotifyIndices; i++)
    {
        //
        // Soldier on if removal of one notification index fails, but preserve
        // the first failure status code.
        //
        NV_STATUS tmpStatus =
            _semsurfRemoveNotifyBinding(pSemSurf,
                                        pChannelNode->gpuIdx,
                                        pChannelNode->notifyIndices[i]);

        if (tmpStatus == NV_OK)
        {
            NV_PRINTF(LEVEL_INFO,
                      "SemSurf(0x%08x, 0x%08x): "
                      "Unbound event for GPU instance 0x%08x notify index 0x%08x\n",
                      RES_GET_CLIENT_HANDLE(pSemSurf), RES_GET_HANDLE(pSemSurf),
                      pChannelNode->gpuIdx, pChannelNode->notifyIndices[i]);
        }
        else if (rmStatus == NV_OK)
        {
            rmStatus = tmpStatus;
        }
    }

    mapRemove(&pSemSurf->boundChannelMap, pChannelNode);
    portMemFree(pChannelNode);

    return rmStatus;
}

static NV_STATUS
semsurfCopyConstruct
(
    SemaphoreSurface *pSemSurf,
    CALL_CONTEXT *pCallContext,
    RS_RES_ALLOC_PARAMS_INTERNAL *pParams
)
{
    RsResourceRef *pSrcRef = pParams->pSrcRef;
    SemaphoreSurface *pSrcSemSurf = dynamicCast(pSrcRef->pResource,
                                                SemaphoreSurface);

    pSemSurf->pShared = pSrcSemSurf->pShared;
    NV_ASSERT(pSemSurf->pShared->refCount > 0);
    pSemSurf->pShared->refCount++;

    mapInitIntrusive(&pSemSurf->boundChannelMap);

    NV_PRINTF(LEVEL_NOTICE,
              "SemSurf(0x%08x, 0x%08x): Copied with SemMem(0x%08x, 0x%08x)\n",
              RES_GET_CLIENT_HANDLE(pSemSurf), RES_GET_HANDLE(pSemSurf),
              pSemSurf->pShared->hClient, pSemSurf->pShared->hSemaphoreMem);

    return NV_OK;
}

static void
_semsurfDestroyShared
(
    SEM_SHARED_DATA *pShared
)
{
    RM_API *pRmApi = rmapiGetInterface(RMAPI_GPU_LOCK_INTERNAL);
    SEM_NOTIFIERIter notIter;

    if (pShared == NULL)
        return;

    //
    // Any notifier entries should have been cleaned up as part of the
    // destructors of the semaphore surface objects sharing this data.
    //
    NV_ASSERT(mapCount(&pShared->notifierMap) == 0);

    for (notIter = mapIterAll(&pShared->notifierMap);
         mapIterNext(&notIter);
         notIter = mapIterAll(&pShared->notifierMap))
    {
        SEM_NOTIFIER_NODE *pNotNode = notIter.pValue;

        portSyncSpinlockAcquire(pShared->pSpinlock);
        mapRemove(&pShared->notifierMap, pNotNode);
        portSyncSpinlockRelease(pShared->pSpinlock);

        _semsurfNodeDestroy(pNotNode);
    }

    mapDestroy(&pShared->notifierMap);
    mapDestroy(&pShared->listenerMap);

    if (pShared->pMaxSubmittedMem)
    {
        if (pShared->maxSubmittedKernAddr)
        {
            pRmApi->UnmapFromCpu(pRmApi,
                                 pShared->hClient,
                                 pShared->hDevice,
                                 pShared->hMaxSubmittedMem,
                                 pShared->maxSubmittedKernAddr,
                                 0, 0);

            pShared->maxSubmittedKernAddr = 0;
            pShared->pMaxSubmitted = NULL;
        }
    }

    if (pShared->pSemaphoreMem)
    {
        if (pShared->semKernAddr)
        {
            pRmApi->UnmapFromCpu(pRmApi,
                                 pShared->hClient,
                                 pShared->hDevice,
                                 pShared->hSemaphoreMem,
                                 pShared->semKernAddr,
                                 0, 0);
        }

        pShared->semKernAddr = 0;
        pShared->pSem = NULL;
    }

    pShared->pMaxSubmittedMem = NULL;
    pShared->pSemaphoreMem = NULL;

    _semsurfFreeMemory(pShared);

    if (pShared->pSpinlock)
        portSyncSpinlockDestroy(pShared->pSpinlock);

    portMemFree(pShared);
}

static NvBool
_semsurfValidateIndex
(
    SEM_SHARED_DATA *pShared,
    NvU64 index
)
{
    if (index < pShared->slotCount)
        return NV_TRUE;
    else
        return NV_FALSE;
}

NV_STATUS
semsurfConstruct_IMPL
(
    SemaphoreSurface *pSemSurf,
    CALL_CONTEXT *pCallContext,
    RS_RES_ALLOC_PARAMS_INTERNAL *pParams
)
{
    RM_API                                 *pRmApi         = rmapiGetInterface(RMAPI_GPU_LOCK_INTERNAL);
    OBJGPU                                 *pGpu           = GPU_RES_GET_GPU(pSemSurf);
    MemoryManager                          *pMemoryManager = GPU_GET_MEMORY_MANAGER(pGpu);
    SEM_SHARED_DATA                        *pShared;
    RmClient                               *pClient;
    RsClient                               *pRsClient;
    NV_SEMAPHORE_SURFACE_ALLOC_PARAMETERS  *pAllocParams = pParams->pAllocParams;
    NvU32                                   maxSubmittedCoherency;
    NV_STATUS                               status = NV_OK;
    NvU64                                   i;

    NV_CHECK_OR_RETURN(LEVEL_INFO, !IsSLIEnabled(pGpu), NV_ERR_INVALID_STATE);

    if (RS_IS_COPY_CTOR(pParams))
    {
        //
        // Copy constructor (NvRmDupObject)
        //
        return semsurfCopyConstruct(pSemSurf, pCallContext, pParams);
    }

    NV_CHECK_OR_RETURN(LEVEL_NOTICE,
                       pAllocParams->flags == 0ULL,
                       NV_ERR_INVALID_ARGUMENT);

    pShared = pSemSurf->pShared = portMemAllocNonPaged(sizeof(*pSemSurf->pShared));
    NV_ASSERT_TRUE_OR_GOTO(status, pShared != NULL, NV_ERR_NO_MEMORY, ctorFailed);

    portMemSet(pShared, 0, sizeof(*pShared));
    pShared->refCount = 1;
    mapInitIntrusive(&pShared->listenerMap);
    mapInitIntrusive(&pShared->notifierMap);
    mapInitIntrusive(&pSemSurf->boundChannelMap);

    pShared->pSpinlock = portSyncSpinlockCreate(portMemAllocatorGetGlobalNonPaged());
    NV_ASSERT_TRUE_OR_GOTO(status, pShared->pSpinlock != NULL, NV_ERR_NO_MEMORY, ctorFailed);

    NV_ASSERT_OK_OR_GOTO(status,
        memmgrGetInternalClientHandles(pGpu, pMemoryManager, GPU_RES_GET_DEVICE(pSemSurf),
                                            &pShared->hClient, &pShared->hDevice, &pShared->hSubdevice),
    ctorFailed);

    NV_ASSERT_OK_OR_GOTO(status,
        pRmApi->Control(pRmApi,
                        pShared->hClient,
                        pShared->hSubdevice,
                        NV2080_CTRL_CMD_FB_GET_SEMAPHORE_SURFACE_LAYOUT,
                       &pShared->layout,
                        sizeof pShared->layout),
    ctorFailed);

    pShared->bIs64Bit           = !!(pShared->layout.caps & NV2080_CTRL_FB_GET_SEMAPHORE_SURFACE_LAYOUT_CAPS_64BIT_SEMAPHORES_SUPPORTED);
    pShared->bHasMonitoredFence = !!(pShared->layout.caps & NV2080_CTRL_FB_GET_SEMAPHORE_SURFACE_LAYOUT_CAPS_MONITORED_FENCE_SUPPORTED);

    pClient = serverutilGetClientUnderLock(pShared->hClient);
    NV_ASSERT_TRUE_OR_GOTO(status, pClient != NULL, NV_ERR_INVALID_STATE, ctorFailed);

    pRsClient = staticCast(pClient, RsClient);

    NV_CHECK_OK_OR_GOTO(status, LEVEL_NOTICE,
        _semsurfDupMemory(pSemSurf, pAllocParams),
        ctorFailed);

    NV_ASSERT_OK_OR_GOTO(status, memGetByHandle(pRsClient, pShared->hSemaphoreMem, &pShared->pSemaphoreMem), ctorFailed);

    /*
     * XXX Limit semaphore surfaces to sysmem for the time being. Vidmem
     * access latency may be too high to allow handling semaphore surfaces
     * directly in the lockless top-half ISR.
     */
    NV_CHECK_OR_ELSE(LEVEL_ERROR,
                     DRF_VAL(OS32, _ATTR, _LOCATION,
                             pShared->pSemaphoreMem->Attr) ==
                     NVOS32_ATTR_LOCATION_PCI,
                     status = NV_ERR_INVALID_PARAMETER;
                     goto ctorFailed);

    NV_ASSERT_OK_OR_GOTO(status,
        pRmApi->MapToCpu(pRmApi,
                         pShared->hClient,
                         pShared->hDevice,
                         pShared->hSemaphoreMem,
                         0,
                         pShared->pSemaphoreMem->pMemDesc->Size,
                         &pShared->semKernAddr,
                         0),
        ctorFailed);

    pShared->slotCount = pShared->pSemaphoreMem->pMemDesc->Size / pShared->layout.size;

    pShared->pSem = KERNEL_POINTER_FROM_NvP64(NvU8 *, pShared->semKernAddr);

    if (!pShared->bIs64Bit)
    {
        if (pShared->hSemaphoreMem != pShared->hMaxSubmittedMem)
        {

            NV_ASSERT_OK_OR_GOTO(status,
                memGetByHandle(pRsClient, pShared->hMaxSubmittedMem, &pShared->pMaxSubmittedMem),
                ctorFailed);

            NV_CHECK_OR_ELSE(LEVEL_ERROR,
                             pShared->pMaxSubmittedMem->pMemDesc->Size >=
                             pShared->pSemaphoreMem->pMemDesc->Size,
                             status = NV_ERR_BUFFER_TOO_SMALL;
                             goto ctorFailed);

            NV_ASSERT_OK_OR_GOTO(status,
                pRmApi->MapToCpu(pRmApi,
                                 pShared->hClient,
                                 pShared->hDevice,
                                 pShared->hMaxSubmittedMem,
                                 0,
                                 pShared->pMaxSubmittedMem->pMemDesc->Size,
                                 &pShared->maxSubmittedKernAddr,
                                 0),
                ctorFailed);

            pShared->slotCount =
                NV_MIN(pShared->slotCount, pShared->pMaxSubmittedMem->pMemDesc->Size / pShared->layout.size);

            pShared->pMaxSubmitted =
                KERNEL_POINTER_FROM_NvP64(NvU8 *, pShared->maxSubmittedKernAddr);
        }
        else
        {
            pShared->pMaxSubmitted = pShared->pSem;
            pShared->pMaxSubmittedMem = pShared->pSemaphoreMem;
        }

        /*
         * XXX Limit semaphore surfaces to sysmem for the time being. Vidmem
         * access latency may be too high to allow handling semaphore surfaces
         * directly in the lockless top-half ISR.
         */
        NV_CHECK_OR_ELSE(LEVEL_ERROR,
                         DRF_VAL(OS32, _ATTR, _LOCATION,
                                 pShared->pMaxSubmittedMem->Attr) ==
                         NVOS32_ATTR_LOCATION_PCI,
                         status = NV_ERR_INVALID_PARAMETER;
                         goto ctorFailed);

        /*
         * The max submitted value memory must be usable with CPU atomics,
         * which at least on ARM architectures requires cached mappings.
         */
        maxSubmittedCoherency = DRF_VAL(OS32, _ATTR, _COHERENCY, pShared->pMaxSubmittedMem->Attr);
        NV_CHECK_OR_ELSE(LEVEL_ERROR,
                         (maxSubmittedCoherency != NVOS32_ATTR_COHERENCY_UNCACHED) &&
                         (maxSubmittedCoherency != NVOS32_ATTR_COHERENCY_WRITE_COMBINE),
                         status = NV_ERR_INVALID_PARAMETER;
                         goto ctorFailed);
    }

    for (i = 0; _semsurfValidateIndex(pShared, i); i++)
    {
        _semsurfSetMonitoredValue(pShared, i, NV_U64_MAX);
    }

    /* Any failures should have already taken the ctorFailed path */
    NV_ASSERT_OR_GOTO(status == NV_OK, ctorFailed);

    NV_PRINTF(LEVEL_INFO,
              "SemSurf(0x%08x, 0x%08x): Constructed with SemMem(0x%08x, 0x%08x)\n",
              RES_GET_CLIENT_HANDLE(pSemSurf), RES_GET_HANDLE(pSemSurf),
              pShared->hClient, pShared->hSemaphoreMem);

    return NV_OK;

ctorFailed:
    mapDestroy(&pSemSurf->boundChannelMap);
    _semsurfDestroyShared(pShared);

    return status;
}

void
semsurfDestruct_IMPL
(
    SemaphoreSurface *pSemSurf
)
{
    SEM_SHARED_DATA *pShared = pSemSurf->pShared;
    SEM_INDEX_LISTENERS_NODE *pIndexListeners;
    SEM_VALUE_LISTENERS_NODE *pValueListeners;
    SEM_VALUE_LISTENERS_NODE *pNextValueListeners;
    SEM_CHANNEL_NODE *pChannelNode;
    EVENTNOTIFICATION *pListener;
    EVENTNOTIFICATION *pNextListener;
    NvU64 minWaitValue;
    NvU64 curIdx;
    NvHandle hSemClient = RES_GET_CLIENT_HANDLE(pSemSurf);
    NvHandle hSemSurf = RES_GET_HANDLE(pSemSurf);
    NvHandle hSharedClient = pShared->hClient;
    NvHandle hSharedMem = pShared->hSemaphoreMem;
    PORT_UNREFERENCED_VARIABLE(hSharedClient);
    PORT_UNREFERENCED_VARIABLE(hSharedMem);

    NV_ASSERT_OR_RETURN_VOID(pShared);
    NV_ASSERT_OR_GOTO(pShared->pSpinlock, skipRemoveListeners);

    NV_PRINTF(LEVEL_INFO,
              "SemSurf(0x%08x, 0x%08x): Destructor with SemMem(0x%08x, 0x%08x)\n",
              hSemClient, hSemSurf, hSharedClient, hSharedMem);

    /* Remove any pending waiters instantiated via this sibling */
    NV_PRINTF(LEVEL_INFO, "SemMem(0x%08x, 0x%08x): Entering spinlock\n",
              hSharedClient, hSharedMem);

    portSyncSpinlockAcquire(pShared->pSpinlock);

    for (pIndexListeners = mapFindGEQ(&pShared->listenerMap, 0);
         pIndexListeners;
         pIndexListeners = mapFindGEQ(&pShared->listenerMap, curIdx + 1))
    {
        minWaitValue = NV_U64_MAX;
        curIdx = mapKey(&pShared->listenerMap, pIndexListeners);

        for (pValueListeners = listHead(&pIndexListeners->listeners);
             pValueListeners;
             pValueListeners = pNextValueListeners)
        {
            for (pListener = pValueListeners->pListeners;
                 pListener;
                 pListener = pNextListener)
            {
                pNextListener = pListener->Next;

                if ((pListener->hEventClient == hSemClient) &&
                    (pListener->hEvent == hSemSurf))
                {
                    NV_PRINTF(LEVEL_WARNING,
                              "SemSurf(0x%08x, 0x%08x): "
                              "Deleting active waiter at index %" NvU64_fmtu
                              " value %" NvU64_fmtu "\n",
                              hSemClient,
                              hSemSurf,
                              curIdx,
                              pValueListeners->value);

                    unregisterEventNotificationWithData(&pValueListeners->pListeners,
                                                        hSemClient,
                                                        NV01_NULL_OBJECT, /* hNotifier/subdevice */
                                                        hSemSurf,
                                                        NV_TRUE, /* match data/notificationHandle */
                                                        pListener->Data);
                }
            }

            pNextValueListeners = listNext(&pIndexListeners->listeners,
                                           pValueListeners);

            if (!pValueListeners->pListeners &&
                (pValueListeners->newValue == 0))
            {
                listRemove(&pIndexListeners->listeners, pValueListeners);
                portMemFree(pValueListeners);
            }
            else if (pValueListeners->value < minWaitValue)
            {
                minWaitValue = pValueListeners->value;
            }
        }

        _semsurfSetMonitoredValue(pShared,
                                  curIdx,
                                  minWaitValue);

        if (listCount(&pIndexListeners->listeners) == 0)
        {
            NV_ASSERT(minWaitValue == NV_U64_MAX);
            mapRemove(&pShared->listenerMap, pIndexListeners);
            portMemFree(pIndexListeners);
        }
    }

    _semsurfDisarmAllIfIdle(pShared);

    portSyncSpinlockRelease(pShared->pSpinlock);
    NV_PRINTF(LEVEL_INFO, "SemMem(0x%08x, 0x%08x): Exited spinlock\n",
              hSharedClient, hSharedMem);

    while ((pChannelNode = mapFindGEQ(&pSemSurf->boundChannelMap, 0)))
    {
        (void)_semsurfUnbindChannel(pSemSurf, pChannelNode);
    }
    mapDestroy(&pSemSurf->boundChannelMap);
skipRemoveListeners:
    NV_ASSERT(pShared->refCount > 0);
    --pShared->refCount;
    if (pShared->refCount <= 0)
        _semsurfDestroyShared(pShared);
}

NV_STATUS
semsurfCtrlCmdRefMemory_IMPL
(
    SemaphoreSurface *pSemSurf,
    NV_SEMAPHORE_SURFACE_CTRL_REF_MEMORY_PARAMS *pParams
)
{
    RM_API *pRmApi = rmapiGetInterface(RMAPI_GPU_LOCK_INTERNAL);
    SEM_SHARED_DATA *pShared = pSemSurf->pShared;
    NV_STATUS status = NV_OK;
    NvHandle hSemMemOut = pParams->hSemaphoreMem;
    NvHandle hMaxMemOut = pParams->hMaxSubmittedMem;
    NvHandle hDeviceDst = RES_GET_HANDLE(GPU_RES_GET_DEVICE(pSemSurf));
    NvBool bSemMemDuped = NV_FALSE;
    NvBool bMaxMemDuped = NV_FALSE;

    NV_CHECK_OK_OR_GOTO(status,
                        LEVEL_ERROR,
                        pRmApi->DupObject(pRmApi,
                                          RES_GET_CLIENT_HANDLE(pSemSurf),
                                          hDeviceDst,
                                          &hSemMemOut,
                                          pShared->hClient,
                                          pShared->hSemaphoreMem,
                                          0),
                        error);

    bSemMemDuped = NV_TRUE;

    if (pShared->pMaxSubmittedMem)
    {
        if (pShared->pMaxSubmittedMem != pShared->pSemaphoreMem)
        {
            NV_CHECK_OK_OR_GOTO(status,
                                LEVEL_ERROR,
                                pRmApi->DupObject(pRmApi,
                                                  RES_GET_CLIENT_HANDLE(pSemSurf),
                                                  hDeviceDst,
                                                  &hMaxMemOut,
                                                  pShared->hClient,
                                                  pShared->hMaxSubmittedMem,
                                                  0),
                                error);

            bMaxMemDuped = NV_TRUE;
        }
        else
        {
            if (pParams->hMaxSubmittedMem != pParams->hSemaphoreMem)
            {
                status = NV_ERR_INVALID_PARAMETER;
                goto error;
            }

            hMaxMemOut = hSemMemOut;
        }
    }
    else
    {
        if (pParams->hMaxSubmittedMem != NV01_NULL_OBJECT)
        {
            return NV_ERR_INVALID_PARAMETER;
        }
    }

    pParams->hSemaphoreMem = hSemMemOut;
    pParams->hMaxSubmittedMem = hMaxMemOut;

    return NV_OK;

error:
    if (bMaxMemDuped)
    {
        pRmApi->Free(pRmApi,
                     RES_GET_CLIENT_HANDLE(pSemSurf),
                     hMaxMemOut);
    }

    if (bSemMemDuped)
    {
        pRmApi->Free(pRmApi,
                     RES_GET_CLIENT_HANDLE(pSemSurf),
                     hSemMemOut);
    }

    return status;
}

NV_STATUS
semsurfCtrlCmdBindChannel_IMPL
(
    SemaphoreSurface *pSemSurf,
    NV_SEMAPHORE_SURFACE_CTRL_BIND_CHANNEL_PARAMS *pParams
)
{
    SEM_NOTIFIER_NODE *pNotNode;
    SEM_CHANNEL_NODE *pChannelNode;
    OBJGPU *pGpu;
    RsClient *pRsClient = staticCast(RES_GET_CLIENT(pSemSurf), RsClient);
    KernelChannel *pKernelChannel;
    NvU64 mapKey;
    NV_STATUS rmStatus = NV_OK;
    NvU32 gpuIdx;
    NvU32 i, j;

    // The channel isn't actually used for anything here yet. However, validate
    // it to ensure using it in the future (E.g., for robust channel error
    // notification registration) won't break buggy clients.
    NV_CHECK_OK_OR_RETURN(LEVEL_INFO, CliGetKernelChannel(pRsClient,
                                                          pParams->hChannel,
                                                          &pKernelChannel));

    pGpu = GPU_RES_GET_GPU(pKernelChannel);

    NV_CHECK_OR_RETURN(LEVEL_INFO, !IsSLIEnabled(pGpu), NV_ERR_INVALID_STATE);

    gpuIdx = gpuGetInstance(pGpu);

    if (pParams->numNotifyIndices > NV_ARRAY_ELEMENTS(pParams->notifyIndices))
    {
        return NV_ERR_INVALID_PARAMETER;
    }

    for (i = 0; i < pParams->numNotifyIndices; i++)
    {
        mapKey = ((NvU64)gpuIdx << 32) | pParams->notifyIndices[i];
        pNotNode = mapFind(&pSemSurf->pShared->notifierMap, mapKey);

        if (pNotNode != NULL)
        {
            NV_ASSERT(pNotNode->nUsers >= 1);

            if (pNotNode->nUsers == NV_U32_MAX)
            {
                NV_PRINTF(LEVEL_WARNING,
                          "SemSurf(0x%08x, 0x%08x): "
                          "GPU instance 0x%08x notify index 0x%08x number of bound channels at max\n",
                          RES_GET_CLIENT_HANDLE(pSemSurf),
                          RES_GET_HANDLE(pSemSurf),
                          gpuIdx, pParams->notifyIndices[i]);

                rmStatus = NV_ERR_INVALID_STATE;
                goto undoBound;
            }

            pNotNode->nUsers++;

            NV_PRINTF(LEVEL_INFO,
                      "SemSurf(0x%08x, 0x%08x): "
                      "Bound to existing event for GPU instance 0x%08x notify index 0x%08x\n",
                      RES_GET_CLIENT_HANDLE(pSemSurf),
                      RES_GET_HANDLE(pSemSurf),
                      gpuIdx, pParams->notifyIndices[i]);

            continue;
        }

        pNotNode = portMemAllocNonPaged(sizeof(*pNotNode));
        if (pNotNode == NULL)
        {
            NV_PRINTF(LEVEL_WARNING,
                      "SemSurf(0x%08x, 0x%08x): "
                      "Failed to allocate an event notifier map node\n",
                      RES_GET_CLIENT_HANDLE(pSemSurf),
                      RES_GET_HANDLE(pSemSurf));

            rmStatus = NV_ERR_NO_MEMORY;
            goto undoBound;
        }
        portMemSet(pNotNode, 0, sizeof(*pNotNode));

        rmStatus = _semsurfDispatcherGet(pGpu,
                                         pKernelChannel,
                                         pParams->notifyIndices[i],
                                         &pNotNode->pDispatcher);
        if (rmStatus != NV_OK)
        {
            portMemFree(pNotNode);
            goto undoBound;
        }

        pNotNode->pShared = pSemSurf->pShared;
        pNotNode->nUsers = 1;

        portSyncSpinlockAcquire(pSemSurf->pShared->pSpinlock);

        if (!mapInsertExisting(&pSemSurf->pShared->notifierMap,
                               mapKey,
                               pNotNode))
        {
            portSyncSpinlockRelease(pSemSurf->pShared->pSpinlock);

            NV_PRINTF(LEVEL_WARNING,
                      "SemSurf(0x%08x, 0x%08x): "
                      "Duplicate entry found for new event notifier map node\n",
                      RES_GET_CLIENT_HANDLE(pSemSurf),
                      RES_GET_HANDLE(pSemSurf));

            _semsurfDispatcherPut(pNotNode->pDispatcher);
            portMemFree(pNotNode);
            rmStatus = NV_ERR_INVALID_STATE;
            goto undoBound;
        }

        // Keep the invariant: a surface with listeners has all bindings armed
        if (mapCount(&pSemSurf->pShared->listenerMap) != 0)
            _semsurfNodeArm(pNotNode);

        portSyncSpinlockRelease(pSemSurf->pShared->pSpinlock);

        NV_PRINTF(LEVEL_INFO,
                  "SemSurf(0x%08x, 0x%08x): "
                  "Bound to new event for GPU instance 0x%08x notify index 0x%08x\n",
                  RES_GET_CLIENT_HANDLE(pSemSurf), RES_GET_HANDLE(pSemSurf),
                  gpuIdx, pParams->notifyIndices[i]);
    }

    pChannelNode = portMemAllocNonPaged(sizeof(*pChannelNode));
    if (pChannelNode == NULL)
    {
        NV_PRINTF(LEVEL_WARNING,
                  "SemSurf(0x%08x, 0x%08x): "
                  "Failed to allocate an channel binding map node for channel 0x%08x\n",
                  RES_GET_CLIENT_HANDLE(pSemSurf),
                  RES_GET_HANDLE(pSemSurf),
                  pParams->hChannel);

        rmStatus = NV_ERR_NO_MEMORY;
        goto undoBound;
    }

    pChannelNode->gpuIdx = gpuIdx;
    pChannelNode->numNotifyIndices = pParams->numNotifyIndices;
    (void)portMemCopy(pChannelNode->notifyIndices,
                      pChannelNode->numNotifyIndices *
                      sizeof(pChannelNode->notifyIndices[0]),
                      pParams->notifyIndices,
                      pParams->numNotifyIndices *
                      sizeof(pParams->notifyIndices[0]));

    if (!mapInsertExisting(&pSemSurf->boundChannelMap, pParams->hChannel, pChannelNode))
    {
        NV_PRINTF(LEVEL_WARNING,
                  "SemSurf(0x%08x, 0x%08x): "
                  "Attempt to register duplicate channel binding for channel 0x%08x\n",
                  RES_GET_CLIENT_HANDLE(pSemSurf),
                  RES_GET_HANDLE(pSemSurf),
                  pParams->hChannel);

        portMemFree(pChannelNode);
        rmStatus = NV_ERR_INSERT_DUPLICATE_NAME;
        goto undoBound;
    }

    return NV_OK;

undoBound:
    for (j = 0; j < i; j++)
    {
        _semsurfRemoveNotifyBinding(pSemSurf,
                                    gpuIdx,
                                    pParams->notifyIndices[j]);

    }

    return rmStatus;
}

NV_STATUS
semsurfCtrlCmdUnbindChannel_IMPL
(
    SemaphoreSurface *pSemSurf,
    NV_SEMAPHORE_SURFACE_CTRL_UNBIND_CHANNEL_PARAMS *pParams
)
{
    RsClient *pRsClient = staticCast(RES_GET_CLIENT(pSemSurf), RsClient);
    KernelChannel *pKernelChannel;
    SEM_CHANNEL_NODE *pChannelNode;

    // The channel isn't actually used for anything here yet. However, validate
    // it to ensure using it in the future (E.g., for robust channel error
    // notification registration) won't break buggy clients.
    NV_CHECK_OK_OR_RETURN(LEVEL_INFO, CliGetKernelChannel(pRsClient,
                                                          pParams->hChannel,
                                                          &pKernelChannel));

    if (pParams->numNotifyIndices > NV_ARRAY_ELEMENTS(pParams->notifyIndices))
    {
        return NV_ERR_INVALID_PARAMETER;
    }

    pChannelNode = mapFind(&pSemSurf->boundChannelMap, pParams->hChannel);
    NV_CHECK_OR_RETURN(LEVEL_INFO, pChannelNode != NULL, NV_ERR_INVALID_OBJECT);

    return _semsurfUnbindChannel(pSemSurf, pChannelNode);
}

NV_STATUS
_semsurfSetValueAndNotify
(
    SemaphoreSurface *pSemSurf,
    NvU64 index,
    NvU64 newValue
)
{
    SEM_INDEX_LISTENERS_NODE *valueNode;
    SEM_VALUE_LISTENERSIter vlIter;
    SEM_PENDING_NOTIFICATIONS notifications;
    NvU64 curValue;
    NvU64 minWaitValue;
    NvBool valueChanged = NV_TRUE;

    _semsurfSetValue(pSemSurf->pShared, index, newValue);

    while (valueChanged)
    {
        curValue = newValue;
        listInitIntrusive(&notifications);

        NV_PRINTF(LEVEL_INFO, "SemMem(0x%08x, 0x%08x): Entering spinlock\n",
                  pSemSurf->pShared->hClient, pSemSurf->pShared->hSemaphoreMem);
        portSyncSpinlockAcquire(pSemSurf->pShared->pSpinlock);

        valueNode = mapFind(&pSemSurf->pShared->listenerMap, index);

        if (valueNode)
        {
            minWaitValue = NV_U64_MAX;

            for (vlIter = listIterAll(&valueNode->listeners);
                 listIterNext(&vlIter);
                 vlIter = listIterAll(&valueNode->listeners))
            {
                NV_PRINTF(LEVEL_SILENT,
                          "  Checking index %" NvU64_fmtu " value waiter %"
                          NvU64_fmtu " against semaphore value %" NvU64_fmtu
                          " from CPU write\n",
                          index, vlIter.pValue->value, curValue);

                if (curValue >= vlIter.pValue->value)
                {
                    listInsertExisting(&notifications, NULL, vlIter.pValue);
                    listRemove(&valueNode->listeners, vlIter.pValue);
                    if (vlIter.pValue->newValue != 0)
                    {
                        NV_ASSERT(vlIter.pValue->newValue >= newValue);
                        newValue = vlIter.pValue->newValue;
                    }
                }
                else
                {
                    /* No other values at this index should be signaled yet. */
                    minWaitValue = vlIter.pValue->value;
                    break;
                }
            }

            if (listCount(&valueNode->listeners) == 0)
            {
                NV_ASSERT(minWaitValue == NV_U64_MAX);
                mapRemove(&pSemSurf->pShared->listenerMap, valueNode);
                portMemFree(valueNode);
            }

            _semsurfSetMonitoredValue(pSemSurf->pShared, index, minWaitValue);
            _semsurfDisarmAllIfIdle(pSemSurf->pShared);
        }

        portSyncSpinlockRelease(pSemSurf->pShared->pSpinlock);
        NV_PRINTF(LEVEL_INFO, "SemMem(0x%08x, 0x%08x): Exited spinlock\n",
                  pSemSurf->pShared->hClient, pSemSurf->pShared->hSemaphoreMem);

        // Send notifications outside of spinlock. They have already been removed
        // from the object-wide lists, so their existance is private to this
        // instance of this function now. Hence, no locking is required for this
        // step.
        valueChanged = _semsurfNotifyCompleted(pSemSurf->pShared,
                                               &notifications);

        NV_ASSERT(!valueChanged || (newValue > curValue));
    }

    return NV_OK;
}

static NV_STATUS
_semsurfAddWaiter
(
    SemaphoreSurface *pSemSurf,
    RsClient *pRsClient,
    NvU32 hSemaphoreSurf,
    NvU64 index,
    NvU64 waitValue,
    NvU64 newValue,
    NvP64 notificationHandle,
    NvBool bKernel
)
{
    SEM_INDEX_LISTENERS_NODE *pIndexListeners;
    SEM_VALUE_LISTENERSIter vlIter;
    SEM_VALUE_LISTENERS_NODE *pValueListeners;
    EVENTNOTIFICATION *pListener;
    NvHandle hClient = pRsClient->hClient;
    NvBool valid;
    NV_STATUS rmStatus = NV_OK;
    NvU64 semValue;
    NvU64 prevMinWaitValue = NV_U64_MAX;

    // The new value must be greater than the wait value to guarantee
    // the monotonically incrementing behavior required of semaphore surface
    // values.
    if ((newValue != 0) && (newValue <= waitValue))
    {
        NV_PRINTF(LEVEL_ERROR,
                  "SemSurf(0x%08x, 0x%08x): "
                  "Requested backwards update from %" NvU64_fmtu "->%"
                  NvU64_fmtu " at idx %" NvU64_fmtu "\n",
                  hClient, hSemaphoreSurf, waitValue, newValue, index);
        return NV_ERR_INVALID_STATE;
    }

    NV_PRINTF(LEVEL_INFO, "SemMem(0x%08x, 0x%08x): Entering spinlock\n",
              pSemSurf->pShared->hClient,
              pSemSurf->pShared->hSemaphoreMem);
    portSyncSpinlockAcquire(pSemSurf->pShared->pSpinlock);

    //
    // Arm the surface's bindings before the monitored fence is lowered below,
    // so that the dispatch callback for an interrupt caused by it visits this
    // surface. If the surface already has listeners, they're armed already.
    // Undone at failureUnlock if no listener is left.
    //
    if (mapCount(&pSemSurf->pShared->listenerMap) == 0)
        _semsurfArmAll(pSemSurf->pShared);

    pIndexListeners = mapFind(&pSemSurf->pShared->listenerMap, index);

    if (pIndexListeners && listCount(&pIndexListeners->listeners) > 0)
    {
        pValueListeners = listHead(&pIndexListeners->listeners);
        prevMinWaitValue = pValueListeners->value;
    }

    /* Check if semaphore value has already been reached. This must be done
     * inside the spinlock to prevent the following race from dropping
     * notifications:
     *
     * -Enter RM control registering waiter for value 1
     * -RM control read current semaphore value 0
     * -GPU semaphore write land semaphore value 1
     * -GPU conditional TRAP non-stall interrupt.
     * -RM interrupt handler walks handlers, finds no registered waiters
     * -RM control proceeds, adds waiter for value 1.
     * -No further interrupts are generated.
     * -FAILURE - client's wait stalls indefinitely.
     *
     * Placing the value read inside the spinlock forces this deterministic
     * ordering of the above events:
     *
     * -Enter RM control registering waiter for value 1
     *  *lock*
     * -RM control read current semaphore value 0
     * -GPU semaphore write land semaphore value 1
     * -GPU conditional TRAP non-stall interrupt.
     * -RM interrupt handler walks handlers
     *  *lock* -- Blocks, defers rest of handler
     * -RM control proceeds, adds waiter for value 1.
     *  *unlock
     * -RM interrupt handler walks handlers
     *  *lock* -- Unblocks, defers rest of handler
     * --finds the registered waiter, signals it
     *  *unlock*
     * -SUCCESS - client's wait was signaled.
     *
     * Additionally, note there is a race involving checking the semaphore
     * value and updating the monitored fence/conditional trap value here. In
     * order for the semaphore surface event handler to have a chance to run
     * and queue up a waiter list walk, the monitored fence value must be
     * updated before the conditional trap methods execute. These execute
     * after the methods to update the semaphore value, but the following
     * race is possible if the monitored fence value is updated after the
     * check for already-signalled semaphores:
     *
     * -RM control reads current semaphore value 0
     *  *Semaphore not yet signaled. Proceed with registering a waiter*
     * -GPU semaphore write land semaphore value 1
     * -GPU conditional TRAP executes: Monitored fence value indicates no waiter
     *  *Interrupt is not generated*
     * -RM control updates monitored fence value to 1
     * -RM control finishes registering waiter on value 1
     * -FAILURE - conditional trap method has already executed,
     *  so waiter won't signal until something else generate san interrupt!
     *
     * Hence, enough work must be done to update the monitored fence value
     * before checking for an already signalled semaphore, and then the
     * monitored fence update, if any, must be undone if an already-signalled
     * semaphore is indeed found. If the updated monitored fence value has
     * already caused an interrupt to be generated, that's OK, the handler will
     * just be a no-op.
     */
    if (waitValue < prevMinWaitValue)
    {
        _semsurfSetMonitoredValue(pSemSurf->pShared, index, waitValue);
    }

    semValue = _semsurfGetValue(pSemSurf->pShared, index);

    if (semValue >= waitValue)
    {
        NV_PRINTF(LEVEL_NOTICE,
                  "SemSurf(0x%08x, 0x%08x): "
                  "Detected already signalled wait for %" NvU64_fmtu
                  " at idx %" NvU64_fmtu " current val %" NvU64_fmtu "\n",
                  hClient, hSemaphoreSurf, waitValue, index, semValue);
        rmStatus = NV_ERR_ALREADY_SIGNALLED;
        goto failureUnlock;
    }

    if (!pIndexListeners)
    {
        pIndexListeners = portMemAllocNonPaged(sizeof(*pIndexListeners));
        if (!pIndexListeners)
        {
            NV_PRINTF(LEVEL_ERROR,
                      "SemSurf(0x%08x, 0x%08x): "
                      "Failed to allocate a semaphore index listeners node\n",
                      hClient, hSemaphoreSurf);
            rmStatus = NV_ERR_NO_MEMORY;
            goto failureUnlock;
        }

        listInitIntrusive(&pIndexListeners->listeners);

        if (!mapInsertExisting(&pSemSurf->pShared->listenerMap,
                               index,
                               pIndexListeners))
        {
            NV_PRINTF(LEVEL_ERROR,
                      "SemSurf(0x%08x, 0x%08x): "
                      "Duplicate entry found for new index listener list\n",
                      hClient, hSemaphoreSurf);
            portMemFree(pIndexListeners);
            rmStatus = NV_ERR_INVALID_STATE;
            goto failureUnlock;
        }
    }

    /* XXX Would be easier/faster if the value listener list was a priority queue */
    vlIter = listIterAll(&pIndexListeners->listeners);
    while ((valid = listIterNext(&vlIter)) && vlIter.pValue->value < waitValue);

    if (valid && vlIter.pValue->value == waitValue)
    {
        pValueListeners = vlIter.pValue;
    }
    else
    {
        pValueListeners = portMemAllocNonPaged(sizeof(*pValueListeners));

        if (!pValueListeners)
        {
            NV_PRINTF(LEVEL_ERROR,
                      "SemSurf(0x%08x, 0x%08x): "
                      "Failed to allocate a semaphore value listener node\n",
                      hClient, hSemaphoreSurf);
            rmStatus = NV_ERR_NO_MEMORY;
            goto cleanupIndexListener;
        }
        portMemSet(pValueListeners, 0, sizeof(*pValueListeners));

        pValueListeners->value = waitValue;
        pValueListeners->index = index;

        listInsertExisting(&pIndexListeners->listeners,
                           valid ? vlIter.pValue : NULL, pValueListeners);
    }

    if (newValue)
    {
        // It is a client error if two waiters request to auto-update the value
        // of a semaphore after it reaches the same prerequisite value, as the
        // order of those signal two operations is indeterminate. This could be
        // handled by taking the max here without violating any forward progress
        // rules, but it is better to return an error given the likelyhood there
        // is an error in the client's logic.
        if ((pValueListeners->newValue != 0))
        {
            NV_PRINTF(LEVEL_ERROR,
                      "SemSurf(0x%08x, 0x%08x): "
                      "Existing value-updating waiter at index %" NvU64_fmtu
                      " for wait value %" NvU64_fmtu ":\n Existing update "
                      "value: %" NvU64_fmtu "\n Requested update value: %"
                      NvU64_fmtu "\n",
                      hClient, hSemaphoreSurf, index, waitValue,
                      pValueListeners->newValue, newValue);
            rmStatus = NV_ERR_STATE_IN_USE;
            goto cleanupValueListener;

        }
        pValueListeners->newValue = newValue;
    }

    if (notificationHandle)
    {
        for (pListener = pValueListeners->pListeners;
             pListener;
             pListener = pListener->Next)
        {
            if (pListener->Data == notificationHandle)
            {
                NV_PRINTF(LEVEL_ERROR,
                          "SemSurf(0x%08x, 0x%08x): "
                          "Notification handle already registered at index %"
                          NvU64_fmtu " for wait value %" NvU64_fmtu ".\n",
                          hClient, hSemaphoreSurf, index, waitValue);
                /*
                 * Back out the auto-update value applied above, if any. Without
                 * one, a value that is set belongs to another waiter.
                 */
                if (newValue != 0)
                    pValueListeners->newValue = 0;
                else if (pValueListeners->newValue != 0)
                    os_fix_hit_event(NV_FIX_HIT_SEMSURF_WAITER_KEPT, 0);
                rmStatus = NV_ERR_STATE_IN_USE;
                goto cleanupValueListener;
            }
        }

        rmStatus = registerEventNotification(&pValueListeners->pListeners,
                                             pRsClient,
                                             NV01_NULL_OBJECT, /* hNotifier/subdevice */
                                             hSemaphoreSurf,
                                             NV_SEMAPHORE_SURFACE_WAIT_VALUE |
                                             NV01_EVENT_WITHOUT_EVENT_DATA,
                                             /*
                                              * Allow user to pass this in
                                              * explicitly?  Doesn't seem to provide
                                              * any added value value and adds more
                                              * parameter validation work
                                              */
                                             bKernel ?
                                             NV01_EVENT_KERNEL_CALLBACK_EX :
                                             NV01_EVENT_WIN32_EVENT,
                                             notificationHandle,
                                             !bKernel);

        if (rmStatus != NV_OK)
        {
            NV_PRINTF(LEVEL_ERROR,
                      "SemSurf(0x%08x, 0x%08x): "
                      "Failed to register event notification for semaphore surface "
                      "listener at index %" NvU64_fmtu ", value %" NvU64_fmtu
                      ".  Status: 0x%08x\n",
                      hClient, hSemaphoreSurf, index, waitValue, rmStatus);
            /* Back out the auto-update value applied above, if any */
            if (newValue != 0)
                pValueListeners->newValue = 0;
            goto cleanupValueListener;
        }
    }

    portSyncSpinlockRelease(pSemSurf->pShared->pSpinlock);
    NV_PRINTF(LEVEL_INFO, "SemMem(0x%08x, 0x%08x): Exited spinlock\n",
              pSemSurf->pShared->hClient, pSemSurf->pShared->hSemaphoreMem);

    NV_PRINTF(LEVEL_INFO,
              "SemSurf(0x%08x, 0x%08x): "
              "Registered semaphore surface value listener at index %"
              NvU64_fmtu ", value %" NvU64_fmtu " current value %" NvU64_fmtu
              " post-wait value %" NvU64_fmtu " notification: " NvP64_fmt "\n",
              hClient, hSemaphoreSurf, index, waitValue, semValue, newValue,
              notificationHandle);

    return rmStatus;

cleanupValueListener:
    /* The node may be another waiter's, which can be only an auto-update */
    if (!pValueListeners->pListeners &&
        (pValueListeners->newValue == 0))
    {
        listRemove(&pIndexListeners->listeners, pValueListeners);
        portMemFree(pValueListeners);
    }
    else if (!pValueListeners->pListeners)
    {
        // Kept: an update-only node of another waiter
        os_fix_hit_event(NV_FIX_HIT_SEMSURF_WAITER_KEPT, 0);
    }

cleanupIndexListener:
    if (listCount(&pIndexListeners->listeners) == 0)
    {
       mapRemove(&pSemSurf->pShared->listenerMap, pIndexListeners);
       portMemFree(pIndexListeners);
    }

failureUnlock:
    // Must be done with the lock.
    if (waitValue < prevMinWaitValue)
    {
        _semsurfSetMonitoredValue(pSemSurf->pShared, index, prevMinWaitValue);
    }

    _semsurfDisarmAllIfIdle(pSemSurf->pShared);

    portSyncSpinlockRelease(pSemSurf->pShared->pSpinlock);
    NV_PRINTF(LEVEL_INFO, "SemMem(0x%08x, 0x%08x): Exited spinlock\n",
              pSemSurf->pShared->hClient, pSemSurf->pShared->hSemaphoreMem);

    // There's no point of going through the trouble of notifying the waiter in
    // this case, but it is worth immediately running the auto-update code here
    // to avoid the need for the caller to call back in through the whole RMAPI
    // framework with the set value control to emulate an auto-update itself.
    // Just take care of it here.
    if ((rmStatus == NV_ERR_ALREADY_SIGNALLED) && (newValue != 0))
    {
        // On success, the ALREADY_SIGNALLED value must be preserved if the
        // client also asked to register an OS event/callback. If no event/
        // callback was requested, return success, as there is all requested
        // operations have been carried out.
        //
        // On error, the client must always be notified this call did nothing.
        NV_STATUS setValStatus = _semsurfSetValueAndNotify(pSemSurf,
                                                           index,
                                                           newValue);

        if ((setValStatus != NV_OK) || !notificationHandle)
            rmStatus = setValStatus;
    }

    return rmStatus;
}

NV_STATUS
semsurfCtrlCmdRegisterWaiter_IMPL
(
    SemaphoreSurface *pSemSurf,
    NV_SEMAPHORE_SURFACE_CTRL_REGISTER_WAITER_PARAMS *pParams
)
{
    CALL_CONTEXT       *pCallContext = resservGetTlsCallContext();
    NvP64               notificationHandle = 0;
    const NvBool        bKernel = (pCallContext->secInfo.paramLocation ==
                                   PARAM_LOCATION_KERNEL);
    NV_STATUS           rmStatus = NV_OK;

    NV_CHECK_OR_RETURN(LEVEL_ERROR,
                       _semsurfValidateIndex(pSemSurf->pShared, pParams->index),
                       NV_ERR_INVALID_INDEX);

    if (pParams->notificationHandle)
    {
        if (bKernel)
        {
            notificationHandle = (NvP64)pParams->notificationHandle;
        }
        else
        {
            rmStatus = osGetAndRefEventFromUserHandle(
                pCallContext->pClient->hClient,
                (NvP64)pParams->notificationHandle,
                &notificationHandle);
        }
    }

    if (rmStatus != NV_OK)
    {
        NV_PRINTF(LEVEL_ERROR,
                  "Invalid semaphore surface notification handle: 0x%016"
                  NvU64_fmtx ", status: %s (0x%08x)\n",
                  pParams->notificationHandle, nvstatusToString(rmStatus),
                  rmStatus);
        return rmStatus;
    }

    rmStatus = _semsurfAddWaiter(pSemSurf,
                                 staticCast(RES_GET_CLIENT(pSemSurf), RsClient),
                                 RES_GET_HANDLE(pSemSurf),
                                 pParams->index,
                                 pParams->waitValue,
                                 pParams->newValue,
                                 notificationHandle,
                                 bKernel);

    if (rmStatus != NV_OK && !bKernel && notificationHandle != 0)
    {
        osDereferenceObjectCount(NvP64_VALUE(notificationHandle));
    }

    return rmStatus;
}

NV_STATUS
semsurfCtrlCmdSetValue_IMPL
(
    SemaphoreSurface *pSemSurf,
    NV_SEMAPHORE_SURFACE_CTRL_SET_VALUE_PARAMS *pParams
)
{
    NV_CHECK_OR_RETURN(LEVEL_ERROR,
                       _semsurfValidateIndex(pSemSurf->pShared, pParams->index),
                       NV_ERR_INVALID_INDEX);

    return _semsurfSetValueAndNotify(pSemSurf,
                                     pParams->index,
                                     pParams->newValue);
}

static NV_STATUS
_semsurfDelWaiter
(
    SemaphoreSurface *pSemSurf,
    NvU64 index,
    NvU64 waitValue,
    NvP64 notificationHandle,
    NvBool bKernel
)
{
    SEM_INDEX_LISTENERS_NODE *pIndexListeners;
    SEM_VALUE_LISTENERSIter vlIter;
    SEM_VALUE_LISTENERS_NODE *pValueListeners;
    NvHandle hClient = RES_GET_CLIENT_HANDLE(pSemSurf);
    NvHandle hSemaphoreSurf = RES_GET_HANDLE(pSemSurf);
    NvBool valid;
    NV_STATUS rmStatus = NV_ERR_GENERIC;

    NV_PRINTF(LEVEL_INFO, "SemMem(0x%08x, 0x%08x): Entering spinlock\n",
              pSemSurf->pShared->hClient,
              pSemSurf->pShared->hSemaphoreMem);
    portSyncSpinlockAcquire(pSemSurf->pShared->pSpinlock);

    pIndexListeners = mapFind(&pSemSurf->pShared->listenerMap, index);

    if (!pIndexListeners)
        goto unlockReturn;

    vlIter = listIterAll(&pIndexListeners->listeners);
    while ((valid = listIterNext(&vlIter)) && vlIter.pValue->value < waitValue);

    if (!valid || (vlIter.pValue->value != waitValue))
        goto unlockReturn;

    pValueListeners = vlIter.pValue;

    rmStatus = unregisterEventNotificationWithData(&pValueListeners->pListeners,
                                                   hClient,
                                                   /* hNotifier/subdevice */
                                                   NV01_NULL_OBJECT,
                                                   hSemaphoreSurf,
                                                   /* match notificationHandle */
                                                   NV_TRUE,
                                                   notificationHandle);

    if (rmStatus != NVOS_STATUS_SUCCESS)
        goto unlockReturn;

    NV_PRINTF(LEVEL_INFO, "SemSurf(0x%08x, 0x%08x): "
              "Unregistered event notification " NvP64_fmt
              " from semaphore surface listener at index %" NvU64_fmtu
              ", value %" NvU64_fmtu ".\n",
              hClient, hSemaphoreSurf, notificationHandle, index, waitValue);

    if (!pValueListeners->pListeners &&
        (pValueListeners->newValue == 0))
    {
        listRemove(&pIndexListeners->listeners, pValueListeners);
        portMemFree(pValueListeners);

        if (listCount(&pIndexListeners->listeners) == 0)
        {
            mapRemove(&pSemSurf->pShared->listenerMap, pIndexListeners);
            portMemFree(pIndexListeners);
            _semsurfSetMonitoredValue(pSemSurf->pShared, index, NV_U64_MAX);
            _semsurfDisarmAllIfIdle(pSemSurf->pShared);
        }
        else
        {
            pValueListeners = listHead(&pIndexListeners->listeners);
            _semsurfSetMonitoredValue(pSemSurf->pShared, index,
                                      pValueListeners->value);
        }
    }

    rmStatus = NV_OK;

unlockReturn:
    portSyncSpinlockRelease(pSemSurf->pShared->pSpinlock);
    NV_PRINTF(LEVEL_INFO, "SemMem(0x%08x, 0x%08x): Exited spinlock\n",
              pSemSurf->pShared->hClient, pSemSurf->pShared->hSemaphoreMem);

    return rmStatus;
}

NV_STATUS
semsurfCtrlCmdUnregisterWaiter_IMPL
(
    SemaphoreSurface *pSemSurf,
    NV_SEMAPHORE_SURFACE_CTRL_UNREGISTER_WAITER_PARAMS *pParams
)
{
    CALL_CONTEXT       *pCallContext = resservGetTlsCallContext();
    NvP64               notificationHandle = 0;
    const NvBool        bKernel = (pCallContext->secInfo.paramLocation ==
                                   PARAM_LOCATION_KERNEL);
    NV_STATUS           rmStatus = NV_OK;

    NV_CHECK_OR_RETURN(LEVEL_ERROR,
                       _semsurfValidateIndex(pSemSurf->pShared, pParams->index),
                       NV_ERR_INVALID_INDEX);

    if (pParams->notificationHandle)
    {
        if (bKernel)
        {
            notificationHandle = (NvP64)pParams->notificationHandle;
        }
        else
        {
            rmStatus = osGetAndRefEventFromUserHandle(pCallContext->pClient->hClient,
                                                      (NvP64)pParams->notificationHandle,
                                                      &notificationHandle);
        }
    }

    if (rmStatus != NV_OK)
    {
        NV_PRINTF(LEVEL_ERROR,
                  "Invalid semaphore surface notification handle: 0x%016"
                  NvU64_fmtx ", status: %s (0x%08x)\n",
                  pParams->notificationHandle, nvstatusToString(rmStatus),
                  rmStatus);
        return rmStatus;
    }

    rmStatus = _semsurfDelWaiter(pSemSurf,
                                 pParams->index,
                                 pParams->waitValue,
                                 notificationHandle,
                                 bKernel);

    if (!bKernel && notificationHandle != 0)
    {
        osDereferenceObjectCount(NvP64_VALUE(notificationHandle));
    }

    return rmStatus;
}

NvU64
semsurfGetValue_IMPL
(
    SemaphoreSurface *pSemSurf,
    NvU64 index
)
{
    NV_ASSERT_OR_RETURN(_semsurfValidateIndex(pSemSurf->pShared, index), 0);
    return _semsurfGetValue(pSemSurf->pShared, index);
}

NvBool
semsurfValidateIndex_IMPL
(
    SemaphoreSurface *pSemSurf,
    NvU64 index
)
{
    return _semsurfValidateIndex(pSemSurf->pShared, index);
}
