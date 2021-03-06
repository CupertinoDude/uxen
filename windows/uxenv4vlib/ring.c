#include "uxenv4vlib_private.h"

void uxen_v4v_reregister_all_rings(void)
{
    KLOCK_QUEUE_HANDLE lqh;
    xenv4v_extension_t *pde;
    xenv4v_ring_t *robj;

    pde = uxen_v4v_get_pde();
    if (!pde) return;

    KeAcquireInStackQueuedSpinLock(&pde->ring_lock, &lqh);

    if (!IsListEmpty(&pde->ring_list)) {
        for ( robj = (xenv4v_ring_t *)pde->ring_list.Flink; robj != (xenv4v_ring_t *)&pde->ring_list ; robj = (xenv4v_ring_t *)robj->le.Flink) {
            if (gh_v4v_register_ring(pde, robj) ==
                STATUS_INVALID_DEVICE_REQUEST) {
                /* XXX remove robj from list */
                uxen_v4v_warn(
                    "gh_v4v_register_ring (vm%u:%x vm%u) duplicate ring",
                    robj->ring->id.addr.domain, robj->ring->id.addr.port,
                    robj->ring->id.partner);
            }
        }
    }

    KeReleaseInStackQueuedSpinLock(&lqh);

    uxen_v4v_put_pde(pde);

}

void uxen_v4v_send_read_callbacks(xenv4v_extension_t *pde)
{
    KLOCK_QUEUE_HANDLE lqh;
    xenv4v_ring_t *robj;
    LONG gen;

    KeAcquireInStackQueuedSpinLock(&pde->ring_lock, &lqh);
  again:
    gen = InterlockedExchangeAdd(&pde->ring_gen, 0);

    if (!IsListEmpty(&pde->ring_list)) {
        for ( robj = (xenv4v_ring_t *)pde->ring_list.Flink; robj != (xenv4v_ring_t *)&pde->ring_list ; robj = (xenv4v_ring_t *)robj->le.Flink) {
            if (!robj->direct_access) continue;
            if (robj->ring->rx_ptr == robj->ring->tx_ptr) continue;
            if (robj->callback) {
                KIRQL irql;

                KeReleaseInStackQueuedSpinLock(&lqh);
                KeRaiseIrql(DISPATCH_LEVEL, &irql);
                robj->callback(robj->uxen_ring_handle, robj->callback_data1,
                               robj->callback_data2);
                KeLowerIrql(irql);
                KeAcquireInStackQueuedSpinLock(&pde->ring_lock, &lqh);
                if (gen != InterlockedExchangeAdd(&pde->ring_gen, 0))
                    goto again;
            }
        }
    }

    KeReleaseInStackQueuedSpinLock(&lqh);
}

static int user_map_exception(void)
{
    uxen_v4v_verbose("failed to map ring to userspace");
    return EXCEPTION_CONTINUE_EXECUTION;
}

NTSTATUS uxen_v4v_mapring(xenv4v_ring_t *robj, v4v_mapring_values_t *mr)
{

    if (!robj)
        return STATUS_INVALID_PARAMETER;

    if (!robj->mdl)
        return STATUS_INVALID_PARAMETER;

    if (robj->user_map)
        return STATUS_INVALID_PARAMETER;


    try {
        robj->user_map = (void *) MmMapLockedPagesSpecifyCache(robj->mdl, UserMode, MmCached, NULL, FALSE, NormalPagePriority);
    }
    except (user_map_exception()) {
        robj->user_map = NULL;
    }
    robj->ring_is_mapped = 1;

    if (!robj->user_map)
        return STATUS_INVALID_PARAMETER;

    mr->ring = (v4v_ring_t *) robj->user_map;

    return STATUS_SUCCESS;
}

