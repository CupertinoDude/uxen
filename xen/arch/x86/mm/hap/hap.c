/******************************************************************************
 * arch/x86/mm/hap/hap.c
 *
 * hardware assisted paging
 * Copyright (c) 2007 Advanced Micro Devices (Wei Huang)
 * Parts of this code are Copyright (c) 2007 by XenSource Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <xen/config.h>
#include <xen/types.h>
#include <xen/mm.h>
#include <xen/trace.h>
#include <xen/sched.h>
#include <xen/perfc.h>
#include <xen/irq.h>
#include <xen/domain_page.h>
#include <xen/guest_access.h>
#include <xen/keyhandler.h>
#include <asm/event.h>
#include <asm/page.h>
#include <asm/current.h>
#include <asm/flushtlb.h>
#include <asm/shared.h>
#include <asm/hap.h>
#include <asm/paging.h>
#include <asm/p2m.h>
#include <asm/domain.h>
#include <xen/numa.h>
#ifndef __UXEN__
#include <asm/hvm/nestedhvm.h>
#endif  /* __UXEN__ */
#include <asm/hvm/vmx/vmx.h>
#include <asm/hvm/ax.h>

#include "private.h"

/* Override macros from asm/page.h to make them work with mfn_t */
#undef mfn_to_page
#define mfn_to_page(_m) __mfn_to_page(mfn_x(_m))
#undef mfn_valid
#define mfn_valid(_mfn) __mfn_valid(mfn_x(_mfn))
#undef page_to_mfn
#define page_to_mfn(_pg) _mfn(__page_to_mfn(_pg))

/************************************************/
/*          HAP VRAM TRACKING SUPPORT           */
/************************************************/

static int hap_enable_vram_tracking(struct domain *d)
{
    struct sh_dirty_vram *dirty_vram = d->arch.hvm_domain.dirty_vram;

    if ( !dirty_vram )
        return -EINVAL;

    /* turn on PG_log_dirty bit in paging mode */
    paging_lock(d);
    d->arch.paging.mode |= PG_log_dirty;
    paging_unlock(d);

    /* set l1e entries of P2M table to be read-only. */
    p2m_change_type_range(d, dirty_vram->begin_pfn, dirty_vram->end_pfn, 
                          p2m_ram_rw, p2m_ram_logdirty);

    return 0;
}

static int hap_disable_vram_tracking(struct domain *d)
{
    struct sh_dirty_vram *dirty_vram = d->arch.hvm_domain.dirty_vram;

    if ( !dirty_vram )
        return -EINVAL;

    paging_lock(d);
    d->arch.paging.mode &= ~PG_log_dirty;
    paging_unlock(d);

    /* set l1e entries of P2M table with normal mode */
    p2m_change_type_range(d, dirty_vram->begin_pfn, dirty_vram->end_pfn, 
                          p2m_ram_logdirty, p2m_ram_rw);

    return 0;
}

static void hap_clean_vram_tracking(struct domain *d)
{
    struct sh_dirty_vram *dirty_vram = d->arch.hvm_domain.dirty_vram;

    if ( !dirty_vram )
        return;

    /* set l1e entries of P2M table to be read-only. */
    p2m_change_type_range(d, dirty_vram->begin_pfn, dirty_vram->end_pfn, 
                          p2m_ram_rw, p2m_ram_logdirty);
}

static int hap_enable_vram_tracking_l2(struct domain *d)
{
    struct sh_dirty_vram *dirty_vram = d->arch.hvm_domain.dirty_vram;

    if ( !dirty_vram )
        return -EINVAL;

    /* turn on PG_log_dirty bit in paging mode */
    paging_lock(d);
    d->arch.paging.mode |= PG_log_dirty;
    paging_unlock(d);

    /* set l2e entries of P2M table to be read-only. */
    p2m_change_type_range_l2(d, dirty_vram->begin_pfn, dirty_vram->end_pfn, 
                             p2m_ram_rw, p2m_ram_logdirty);

    return 0;
}

static int hap_disable_vram_tracking_l2(struct domain *d)
{
    struct sh_dirty_vram *dirty_vram = d->arch.hvm_domain.dirty_vram;

    if ( !dirty_vram )
        return -EINVAL;

    paging_lock(d);
    d->arch.paging.mode &= ~PG_log_dirty;
    paging_unlock(d);

    /* set l1e entries of P2M table with normal mode */
    p2m_change_type_range_l2(d, dirty_vram->begin_pfn, dirty_vram->end_pfn, 
                             p2m_ram_logdirty, p2m_ram_rw);

    return 0;
}

static void hap_clean_vram_tracking_l2(struct domain *d)
{
    struct sh_dirty_vram *dirty_vram = d->arch.hvm_domain.dirty_vram;

    if ( !dirty_vram )
        return;

    /* set l1e entries of P2M table to be read-only. */
    p2m_change_type_range_l2(d, dirty_vram->begin_pfn, dirty_vram->end_pfn, 
                             p2m_ram_rw, p2m_ram_logdirty);
}

static void hap_vram_tracking_init(struct domain *d)
{
    struct sh_dirty_vram *dirty_vram = d->arch.hvm_domain.dirty_vram;

    if ( !dirty_vram )
        return;

    /* XXX add dirty_vram->max_end_pfn to indicate upto where the gpfn
     * space is safe to ro protect, then enable 2nd condition below */
    if (!(dirty_vram->begin_pfn & ((1ul << PAGE_ORDER_2M) - 1)) &&
        /* !(dirty_vram->end_pfn & ((1ul << PAGE_ORDER_2M) - 1)) && */
        p2m_get_hostp2m(d)->ro_update_l2_entry)
        paging_log_dirty_init(d, hap_enable_vram_tracking_l2,
                              hap_disable_vram_tracking_l2,
                              hap_clean_vram_tracking_l2);
    else
        paging_log_dirty_init(d, hap_enable_vram_tracking,
                              hap_disable_vram_tracking,
                              hap_clean_vram_tracking);
}

int hap_track_dirty_vram(struct domain *d,
                         unsigned long begin_pfn,
                         unsigned long nr,
                         XEN_GUEST_HANDLE_64(uint8) dirty_bitmap,
                         unsigned long want_events)
{
    long rc = 0;
    struct sh_dirty_vram *dirty_vram = d->arch.hvm_domain.dirty_vram;

    if ( nr )
    {
        if ( paging_mode_log_dirty(d) && dirty_vram )
        {
            if ( begin_pfn != dirty_vram->begin_pfn ||
                 begin_pfn + nr != dirty_vram->end_pfn )
            {
                paging_log_dirty_disable(d);
                dirty_vram->begin_pfn = begin_pfn;
                dirty_vram->end_pfn = begin_pfn + nr;
                rc = paging_log_dirty_enable(d);
                if (rc != 0)
                    goto param_fail;
            }
        }
        else if ( !paging_mode_log_dirty(d) && !dirty_vram )
        {
            dirty_vram =
                (struct sh_dirty_vram *)d->extra_1->hvm_domain_dirty_vram;

            dirty_vram->begin_pfn = begin_pfn;
            dirty_vram->end_pfn = begin_pfn + nr;
            d->arch.hvm_domain.dirty_vram = dirty_vram;
            hap_vram_tracking_init(d);
            rc = paging_log_dirty_enable(d);
            if (rc != 0)
                goto param_fail;
        }
        else
        {
            if ( !paging_mode_log_dirty(d) && dirty_vram )
                rc = -EINVAL;
            else
                rc = -ENXIO;
            goto param_fail;
        }
        dirty_vram->want_events = want_events;
        /* get the bitmap */
        rc = paging_log_dirty_range(d, begin_pfn, nr, dirty_bitmap);
    }
    else
    {
        if ( paging_mode_log_dirty(d) && dirty_vram ) {
            rc = paging_log_dirty_disable(d);
            dirty_vram = d->arch.hvm_domain.dirty_vram = NULL;
        } else
            rc = 0;
    }

    return rc;

param_fail:
    if ( dirty_vram )
    {
        dirty_vram = d->arch.hvm_domain.dirty_vram = NULL;
    }
    return rc;
}

/************************************************/
/*            HAP LOG DIRTY SUPPORT             */
/************************************************/

/* hap code to call when log_dirty is enable. return 0 if no problem found. */
static int hap_enable_log_dirty(struct domain *d)
{
    /* turn on PG_log_dirty bit in paging mode */
    paging_lock(d);
    d->arch.paging.mode |= PG_log_dirty;
    paging_unlock(d);

    /* set l1e entries of P2M table to be read-only. */
    p2m_change_entry_type_global(d, p2m_ram_rw, p2m_ram_logdirty);

    return 0;
}

static int hap_disable_log_dirty(struct domain *d)
{
    paging_lock(d);
    d->arch.paging.mode &= ~PG_log_dirty;
    paging_unlock(d);

    /* set l1e entries of P2M table with normal mode */
    p2m_change_entry_type_global(d, p2m_ram_logdirty, p2m_ram_rw);

    return 0;
}

static void hap_clean_dirty_bitmap(struct domain *d)
{
    /* set l1e entries of P2M table to be read-only. */
    p2m_change_entry_type_global(d, p2m_ram_rw, p2m_ram_logdirty);
}

void hap_logdirty_init(struct domain *d)
{
    struct sh_dirty_vram *dirty_vram = d->arch.hvm_domain.dirty_vram;
    if ( paging_mode_log_dirty(d) && dirty_vram )
    {
        paging_log_dirty_disable(d);
        dirty_vram = d->arch.hvm_domain.dirty_vram = NULL;
    }

    /* Reinitialize logdirty mechanism */
    paging_log_dirty_init(d, hap_enable_log_dirty,
                          hap_disable_log_dirty,
                          hap_clean_dirty_bitmap);
}

#ifndef __UXEN__
/************************************************/
/*             HAP SUPPORT FUNCTIONS            */
/************************************************/
static struct page_info *hap_alloc(struct domain *d)
{
    struct page_info *pg = NULL;
    void *p;

    ASSERT(paging_locked_by_me(d));

    pg = page_list_remove_head(&d->arch.paging.hap.freelist);
    if ( unlikely(!pg) )
        return NULL;

    d->arch.paging.hap.free_pages--;

    p = __map_domain_page(pg);
    ASSERT(p != NULL);
    clear_page(p);
    unmap_domain_page(p);

    return pg;
}

static void hap_free(struct domain *d, mfn_t mfn)
{
    struct page_info *pg = mfn_to_page(mfn);

    ASSERT(paging_locked_by_me(d));

    d->arch.paging.hap.free_pages++;
    page_list_add_tail(pg, &d->arch.paging.hap.freelist);
}

static struct page_info *hap_alloc_p2m_page(struct domain *d)
{
    struct page_info *pg;

    /* This is called both from the p2m code (which never holds the 
     * paging lock) and the log-dirty code (which always does). */
    paging_lock_recursive(d);
    pg = hap_alloc(d);

#if CONFIG_PAGING_LEVELS == 3
    /* Under PAE mode, top-level P2M table should be allocated below 4GB space
     * because the size of h_cr3 is only 32-bit. We use alloc_domheap_pages to
     * force this requirement, and exchange the guaranteed 32-bit-clean
     * page for the one we just hap_alloc()ed. */
    if ( d->arch.paging.hap.p2m_pages == 0
         && mfn_x(page_to_mfn(pg)) >= (1UL << (32 - PAGE_SHIFT)) )
    {
        free_domheap_page(pg);
        pg = alloc_domheap_page(
            NULL, MEMF_bits(32) | MEMF_node(domain_to_node(d)));
        if ( likely(pg != NULL) )
        {
            void *p = __map_domain_page(pg);
            clear_page(p);
            unmap_domain_page(p);
        }
    }
#endif

    if ( likely(pg != NULL) )
    {
        d->arch.paging.hap.total_pages--;
        d->arch.paging.hap.p2m_pages++;
        page_set_owner(pg, d);
        pg->count_info |= 1;
    }

    paging_unlock(d);
    return pg;
}

static void hap_free_p2m_page(struct domain *d, struct page_info *pg)
{
    /* This is called both from the p2m code (which never holds the 
     * paging lock) and the log-dirty code (which always does). */
    paging_lock_recursive(d);

    ASSERT(page_get_owner(pg) == d);
    /* Should have just the one ref we gave it in alloc_p2m_page() */
    if ( (pg->count_info & PGC_count_mask) != 1 ) {
#ifndef __UXEN__
        HAP_ERROR("Odd p2m page %p count c=%#lx t=%"PRtype_info"\n",
                     pg, pg->count_info, pg->u.inuse.type_info);
#else  /* __UXEN__ */
        HAP_ERROR("Odd p2m page %p count c=%#lx\n",
                  pg, pg->count_info);
#endif  /* __UXEN__ */
        WARN();
    }
    pg->count_info &= ~PGC_count_mask;
    /* Free should not decrement domain's total allocation, since
     * these pages were allocated without an owner. */
    page_set_owner(pg, NULL);
    d->arch.paging.hap.p2m_pages--;
    d->arch.paging.hap.total_pages++;
    hap_free(d, page_to_mfn(pg));
    ASSERT(d->arch.paging.hap.p2m_pages >= 0);

    paging_unlock(d);
}

/* Return the size of the pool, rounded up to the nearest MB */
static unsigned int
hap_get_allocation(struct domain *d)
{
    unsigned int pg = d->arch.paging.hap.total_pages
        + d->arch.paging.hap.p2m_pages;

    return ((pg >> (20 - PAGE_SHIFT))
            + ((pg & ((1 << (20 - PAGE_SHIFT)) - 1)) ? 1 : 0));
}

/* Set the pool of pages to the required number of pages.
 * Returns 0 for success, non-zero for failure. */
static unsigned int
hap_set_allocation(struct domain *d, unsigned int pages, int *preempted)
{
    struct page_info *pg;

    ASSERT(paging_locked_by_me(d));

    if ( pages < d->arch.paging.hap.p2m_pages )
        pages = 0;
    else
        pages -= d->arch.paging.hap.p2m_pages;

    while ( d->arch.paging.hap.total_pages != pages )
    {
        if ( d->arch.paging.hap.total_pages < pages )
        {
            /* Need to allocate more memory from domheap */
            pg = alloc_domheap_page(NULL, MEMF_node(domain_to_node(d)));
            if ( pg == NULL )
            {
                HAP_PRINTK("failed to allocate hap pages.\n");
                return -ENOMEM;
            }
            d->arch.paging.hap.free_pages++;
            d->arch.paging.hap.total_pages++;
            page_list_add_tail(pg, &d->arch.paging.hap.freelist);
        }
        else if ( d->arch.paging.hap.total_pages > pages )
        {
            /* Need to return memory to domheap */
            if ( page_list_empty(&d->arch.paging.hap.freelist) )
            {
                HAP_PRINTK("failed to free enough hap pages.\n");
                return -ENOMEM;
            }
            pg = page_list_remove_head(&d->arch.paging.hap.freelist);
            ASSERT(pg);
            d->arch.paging.hap.free_pages--;
            d->arch.paging.hap.total_pages--;
            free_domheap_page(pg);
        }

#ifndef __UXEN__
        /* Check to see if we need to yield and try again */
        if ( preempted && hypercall_preempt_check() )
        {
            *preempted = 1;
            return 0;
        }
#endif  /* __UXEN__ */
    }

    return 0;
}
#else  /* __UXEN__ */
static struct page_info *
hap_alloc_p2m_page(struct domain *d)
{
    struct page_info *pg;
    void *p;

    pg = alloc_domheap_page(NULL, MEMF_host_page);
    if (!pg)
        return NULL;

    page_set_owner(pg, d);
    ASSERT(!(pg->count_info & PGC_count_mask));
    pg->count_info |= 1;

    p = __map_domain_page(pg);
    ASSERT(p != NULL);
    clear_page(p);
    unmap_domain_page(p);

    return pg;
}
static void
hap_free_p2m_page(struct domain *d, struct page_info *pg)
{

    ASSERT((pg->count_info & PGC_count_mask) == 1);
    pg->count_info &= ~PGC_count_mask;
    /* Free should not decrement domain's total allocation, since
     * these pages were allocated without an owner. */
    page_set_owner(pg, NULL);
    free_domheap_page(pg);
}
#endif /* __UXEN__ */

#ifndef __UXEN__
#if CONFIG_PAGING_LEVELS == 4
static void hap_install_xen_entries_in_l4(struct vcpu *v, mfn_t l4mfn)
{
    struct domain *d = v->domain;
    l4_pgentry_t *l4e;

    l4e = hap_map_domain_page(l4mfn);
    ASSERT(l4e != NULL);

    /* Copy the common Xen mappings from the idle domain */
    memcpy(&l4e[ROOT_PAGETABLE_FIRST_XEN_SLOT],
           &idle_pg_table[ROOT_PAGETABLE_FIRST_XEN_SLOT],
           ROOT_PAGETABLE_XEN_SLOTS * sizeof(l4_pgentry_t));

    /* Install the per-domain mappings for this domain */
    l4e[l4_table_offset(PERDOMAIN_VIRT_START)] =
        l4e_from_pfn(mfn_x(page_to_mfn(virt_to_page(d->arch.mm_perdomain_l3))),
                     __PAGE_HYPERVISOR);

    /* Install a linear mapping */
    l4e[l4_table_offset(LINEAR_PT_VIRT_START)] =
        l4e_from_pfn(mfn_x(l4mfn), __PAGE_HYPERVISOR);

    /* Install the domain-specific P2M table */
    l4e[l4_table_offset(RO_MPT_VIRT_START)] =
        l4e_from_pfn(mfn_x(pagetable_get_mfn(p2m_get_pagetable(p2m_get_hostp2m(d)))),
                     __PAGE_HYPERVISOR);

    hap_unmap_domain_page(l4e);
}
#endif /* CONFIG_PAGING_LEVELS == 4 */
#endif  /* __UXEN__ */

#ifndef __UXEN__
#if CONFIG_PAGING_LEVELS == 3
static void hap_install_xen_entries_in_l2h(struct vcpu *v, mfn_t l2hmfn)
{
    struct domain *d = v->domain;
    struct p2m_domain *hostp2m = p2m_get_hostp2m(d);
    l2_pgentry_t *l2e;
    l3_pgentry_t *p2m;
    int i;

    l2e = hap_map_domain_page(l2hmfn);
    ASSERT(l2e != NULL);

    /* Copy the common Xen mappings from the idle domain */
    memcpy(&l2e[L2_PAGETABLE_FIRST_XEN_SLOT & (L2_PAGETABLE_ENTRIES-1)],
           &idle_pg_table_l2[L2_PAGETABLE_FIRST_XEN_SLOT],
           L2_PAGETABLE_XEN_SLOTS * sizeof(l2_pgentry_t));

    /* Install the per-domain mappings for this domain */
    for ( i = 0; i < PDPT_L2_ENTRIES; i++ )
        l2e[l2_table_offset(PERDOMAIN_VIRT_START) + i] =
            l2e_from_pfn(
                mfn_x(page_to_mfn(perdomain_pt_page(d, i))),
                __PAGE_HYPERVISOR);

    /* No linear mapping; will be set up by monitor-table contructor. */
    for ( i = 0; i < 4; i++ )
        l2e[l2_table_offset(LINEAR_PT_VIRT_START) + i] =
            l2e_empty();

    /* Install the domain-specific p2m table */
    ASSERT(pagetable_get_pfn(p2m_get_pagetable(hostp2m)) != 0);
    p2m = hap_map_domain_page(pagetable_get_mfn(p2m_get_pagetable(hostp2m)));
    for ( i = 0; i < MACHPHYS_MBYTES>>1; i++ )
    {
        l2e[l2_table_offset(RO_MPT_VIRT_START) + i] =
            (l3e_get_flags(p2m[i]) & _PAGE_PRESENT)
            ? l2e_from_pfn(mfn_x(_mfn(l3e_get_pfn(p2m[i]))),
                           __PAGE_HYPERVISOR)
            : l2e_empty();
    }
    hap_unmap_domain_page(p2m);
    hap_unmap_domain_page(l2e);
}
#endif

static mfn_t hap_make_monitor_table(struct vcpu *v)
{
    struct domain *d = v->domain;
    struct page_info *pg;

    ASSERT(pagetable_get_pfn(v->arch.monitor_table) == 0);

#if CONFIG_PAGING_LEVELS == 4
    {
        mfn_t m4mfn;
        if ( (pg = hap_alloc(d)) == NULL )
            goto oom;
        m4mfn = page_to_mfn(pg);
        hap_install_xen_entries_in_l4(v, m4mfn);
        return m4mfn;
    }
#elif CONFIG_PAGING_LEVELS == 3
    {
        mfn_t m3mfn, m2mfn;
        l3_pgentry_t *l3e;
        l2_pgentry_t *l2e;
        int i;

        if ( (pg = hap_alloc(d)) == NULL )
            goto oom;
        m3mfn = page_to_mfn(pg);

        /* Install a monitor l2 table in slot 3 of the l3 table.
         * This is used for all Xen entries, including linear maps
         */
        if ( (pg = hap_alloc(d)) == NULL )
            goto oom;
        m2mfn = page_to_mfn(pg);
        l3e = hap_map_domain_page(m3mfn);
        l3e[3] = l3e_from_pfn(mfn_x(m2mfn), _PAGE_PRESENT);
        hap_install_xen_entries_in_l2h(v, m2mfn);
        /* Install the monitor's own linear map */
        l2e = hap_map_domain_page(m2mfn);
        for ( i = 0; i < L3_PAGETABLE_ENTRIES; i++ )
            l2e[l2_table_offset(LINEAR_PT_VIRT_START) + i] =
                (l3e_get_flags(l3e[i]) & _PAGE_PRESENT)
                ? l2e_from_pfn(l3e_get_pfn(l3e[i]), __PAGE_HYPERVISOR)
                : l2e_empty();
        hap_unmap_domain_page(l2e);
        hap_unmap_domain_page(l3e);

        HAP_PRINTK("new monitor table: %#lx\n", mfn_x(m3mfn));
        return m3mfn;
    }
#endif

 oom:
    HAP_ERROR("out of memory building monitor pagetable\n");
    domain_crash(d);
    return _mfn(INVALID_MFN);
}

static void hap_destroy_monitor_table(struct vcpu* v, mfn_t mmfn)
{
    struct domain *d = v->domain;

#if CONFIG_PAGING_LEVELS == 3
    /* Need to destroy the l2 monitor page in slot 4 too */
    {
        l3_pgentry_t *l3e = hap_map_domain_page(mmfn);
        ASSERT(l3e_get_flags(l3e[3]) & _PAGE_PRESENT);
        hap_free(d, _mfn(l3e_get_pfn(l3e[3])));
        hap_unmap_domain_page(l3e);
    }
#endif

    /* Put the memory back in the pool */
    hap_free(d, mmfn);
}
#endif  /* __UXEN__ */

/************************************************/
/*          HAP DOMAIN LEVEL FUNCTIONS          */
/************************************************/
void hap_domain_init(struct domain *d)
{
#ifndef __UXEN__
    INIT_PAGE_LIST_HEAD(&d->arch.paging.hap.freelist);
#endif  /* __UXEN__ */

    hap_logdirty_init(d);
}

/* return 0 for success, -errno for failure */
int hap_enable(struct domain *d, u32 mode)
{
#ifndef __UXEN__
    unsigned int old_pages;
    uint8_t i;
#endif  /* __UXEN__ */
    int rv = 0;

    domain_pause(d);

    /* error check */
    if ( (d == current->domain) )
    {
        rv = -EINVAL;
        goto out;
    }

#ifndef __UXEN__
    old_pages = d->arch.paging.hap.total_pages;
    if ( old_pages == 0 )
    {
        unsigned int r;
        paging_lock(d);
        r = hap_set_allocation(d, 256, NULL);
        paging_unlock(d);
        if ( r != 0 )
        {
            paging_lock(d);
            hap_set_allocation(d, 0, NULL);
            paging_unlock(d);
            rv = -ENOMEM;
            goto out;
        }
    }
#endif  /* __UXEN__ */

    /* Allow p2m and log-dirty code to borrow our memory */
    d->arch.paging.alloc_page = hap_alloc_p2m_page;
    d->arch.paging.free_page = hap_free_p2m_page;

    /* allocate P2m table */
    if ( mode & PG_translate )
    {
        rv = p2m_alloc_table(p2m_get_hostp2m(d));
        if ( rv != 0 )
            goto out;
    }

#ifndef __UXEN__
    for (i = 0; i < MAX_NESTEDP2M; i++) {
        rv = p2m_alloc_table(d->arch.nested_p2m[i]);
        if ( rv != 0 )
           goto out;
    }
#endif  /* __UXEN__ */

    /* Now let other users see the new mode */
    d->arch.paging.mode = mode | PG_HAP_enable;

 out:
    domain_unpause(d);
    return rv;
}

void hap_final_teardown(struct domain *d)
{
#ifndef __UXEN__
    uint8_t i;

    /* Destroy nestedp2m's first */
    for (i = 0; i < MAX_NESTEDP2M; i++) {
        p2m_teardown(d->arch.nested_p2m[i]);
    }

    if ( d->arch.paging.hap.total_pages != 0 )
        hap_teardown(d);
#endif  /* __UXEN__ */

    p2m_teardown(p2m_get_hostp2m(d));
#ifndef __UXEN__
    /* Free any memory that the p2m teardown released */
    paging_lock(d);
    hap_set_allocation(d, 0, NULL);
    ASSERT(d->arch.paging.hap.p2m_pages == 0);
    paging_unlock(d);
#endif  /* __UXEN__ */
}

void hap_teardown(struct domain *d)
{
#ifndef __UXEN__
    struct vcpu *v;
    mfn_t mfn;
#endif  /* __UXEN__ */

    ASSERT(d->is_dying);
    ASSERT(d != current->domain);

    if ( !paging_locked_by_me(d) )
        paging_lock(d); /* Keep various asserts happy */

#ifndef __UXEN__
    if ( paging_mode_enabled(d) )
    {
        /* release the monitor table held by each vcpu */
        for_each_vcpu ( d, v )
        {
            if ( paging_get_hostmode(v) && paging_mode_external(d) )
            {
                mfn = pagetable_get_mfn(v->arch.monitor_table);
                if ( mfn_valid(mfn) && (mfn_x(mfn) != 0) )
                    hap_destroy_monitor_table(v, mfn);
                v->arch.monitor_table = pagetable_null();
            }
        }
    }

    if ( d->arch.paging.hap.total_pages != 0 )
    {
        HAP_PRINTK("teardown of vm%u starts."
                      "  pages total = %u, free = %u, p2m=%u\n",
                      d->domain_id,
                      d->arch.paging.hap.total_pages,
                      d->arch.paging.hap.free_pages,
                      d->arch.paging.hap.p2m_pages);
        hap_set_allocation(d, 0, NULL);
        HAP_PRINTK("teardown of vm%u done."
                      "  pages total = %u, free = %u, p2m=%u\n",
                      d->domain_id,
                      d->arch.paging.hap.total_pages,
                      d->arch.paging.hap.free_pages,
                      d->arch.paging.hap.p2m_pages);
        ASSERT(d->arch.paging.hap.total_pages == 0);
    }
#endif  /* __UXEN__ */

    d->arch.paging.mode &= ~PG_log_dirty;

    paging_unlock(d);
}

#ifndef __UXEN__
int hap_domctl(struct domain *d, xen_domctl_shadow_op_t *sc,
               XEN_GUEST_HANDLE(void) u_domctl)
{
#ifndef __UXEN__
    int rc, preempted = 0;
#endif  /* __UXEN__ */

    switch ( sc->op )
    {
    case XEN_DOMCTL_SHADOW_OP_SET_ALLOCATION:
#ifndef __UXEN__
        paging_lock(d);
        rc = hap_set_allocation(d, sc->mb << (20 - PAGE_SHIFT), &preempted);
        paging_unlock(d);
        if ( preempted )
#ifndef __UXEN__
            /* Not finished.  Set up to re-run the call. */
            rc = hypercall_create_continuation(__HYPERVISOR_domctl, "h",
                                               u_domctl);
#else   /* __UXEN__ */
            BUG();
#endif  /* __UXEN__ */
        else
            /* Finished.  Return the new allocation */
            sc->mb = hap_get_allocation(d);
        return rc;
#else   /* __UXEN__ */
        return 0;
#endif  /* __UXEN__ */
    case XEN_DOMCTL_SHADOW_OP_GET_ALLOCATION:
#ifndef __UXEN__
        sc->mb = hap_get_allocation(d);
#else   /* __UXEN__ */
        sc->mb = 0;
#endif  /* __UXEN__ */
        return 0;
    default:
        HAP_ERROR("Bad hap domctl op %u\n", sc->op);
        return -EINVAL;
    }
}
#endif  /* __UXEN__ */

static const struct paging_mode hap_paging_real_mode;
static const struct paging_mode hap_paging_protected_mode;
static const struct paging_mode hap_paging_pae_mode;
#if CONFIG_PAGING_LEVELS == 4
static const struct paging_mode hap_paging_long_mode;
#endif

void hap_vcpu_init(struct vcpu *v)
{
    v->arch.paging.mode = &hap_paging_real_mode;
#ifndef __UXEN__
    v->arch.paging.nestedmode = &hap_paging_real_mode;
#endif  /* __UXEN__ */
}

/************************************************/
/*          HAP PAGING MODE FUNCTIONS           */
/************************************************/
/*
 * HAP guests can handle page faults (in the guest page tables) without
 * needing any action from Xen, so we should not be intercepting them.
 */
static int hap_page_fault(struct vcpu *v, unsigned long va,
                          struct cpu_user_regs *regs)
{
    struct domain *d = v->domain;

DEBUG();
    HAP_ERROR("Intercepted a guest #PF (vm%u.%u) with HAP enabled.\n",
              d->domain_id, v->vcpu_id);
    domain_crash(d);
    return 0;
}

/*
 * HAP guests can handle invlpg without needing any action from Xen, so
 * should not be intercepting it.
 */
static int hap_invlpg(struct vcpu *v, unsigned long va)
{
DEBUG();
#ifndef __UXEN__
    if (nestedhvm_enabled(v->domain)) {
        /* Emulate INVLPGA:
         * Must perform the flush right now or an other vcpu may
         * use it when we use the next VMRUN emulation, otherwise.
         */
        p2m_flush(v, vcpu_nestedhvm(v).nv_p2m);
        return 1;
    }
#endif  /* __UXEN__ */

    HAP_ERROR("Intercepted a guest INVLPG (vm%u.%u) with HAP enabled.\n",
              v->domain->domain_id, v->vcpu_id);
    domain_crash(v->domain);
    return 0;
}

static int hap_update_cr3(struct vcpu *v, int do_locking)
{
    v->arch.hvm_vcpu.hw_cr[3] = v->arch.hvm_vcpu.guest_cr[3];
    return hvm_update_guest_cr(v, 3);
}

const struct paging_mode *
hap_paging_get_mode(struct vcpu *v)
{
    return !hvm_paging_enabled(v)   ? &hap_paging_real_mode :
#if CONFIG_PAGING_LEVELS == 4
        hvm_long_mode_enabled(v) ? &hap_paging_long_mode :
#endif
        hvm_pae_enabled(v)       ? &hap_paging_pae_mode  :
                                   &hap_paging_protected_mode;
}

static int hap_update_paging_modes(struct vcpu *v)
{
    struct domain *d = v->domain;
    uint64_t cr3;

    paging_lock(d);

    v->arch.paging.mode = hap_paging_get_mode(v);

#ifndef __UXEN__
    if ( pagetable_is_null(v->arch.monitor_table) )
    {
        mfn_t mmfn = hap_make_monitor_table(v);
        v->arch.monitor_table = pagetable_from_mfn(mmfn);
        make_cr3(v, mfn_x(mmfn));
        hvm_update_host_cr3(v);
    }
#else   /* __UXEN__ */
    cr3 = read_cr3();
    if (v->arch.cr3 != cr3) {
        make_cr3(v, cr3);
        hvm_update_host_cr3(v);
    }
#endif  /* __UXEN__ */

    paging_unlock(d);

    /* CR3 is effectively updated by a mode change. Flush ASIDs, etc. */
    return hap_update_cr3(v, 0);
}

#ifndef __UXEN__
#if CONFIG_PAGING_LEVELS == 3
static void p2m_install_entry_in_monitors(struct domain *d, l3_pgentry_t *l3e)
/* Special case, only used for PAE hosts: update the mapping of the p2m
 * table.  This is trivial in other paging modes (one top-level entry
 * points to the top-level p2m, no maintenance needed), but PAE makes
 * life difficult by needing a copy of the p2m table in eight l2h slots
 * in the monitor table.  This function makes fresh copies when a p2m
 * l3e changes. */
{
    l2_pgentry_t *ml2e;
    struct vcpu *v;
    unsigned int index;

DEBUG();
    index = ((unsigned long)l3e & ~PAGE_MASK) / sizeof(l3_pgentry_t);
    ASSERT(index < MACHPHYS_MBYTES>>1);

    for_each_vcpu ( d, v )
    {
        if ( pagetable_get_pfn(v->arch.monitor_table) == 0 )
            continue;

        ASSERT(paging_mode_external(v->domain));

        if ( v == current ) /* OK to use linear map of monitor_table */
            ml2e = __linear_l2_table + l2_linear_offset(RO_MPT_VIRT_START);
        else {
            l3_pgentry_t *ml3e;
            ml3e = hap_map_domain_page(
                pagetable_get_mfn(v->arch.monitor_table));
            ASSERT(l3e_get_flags(ml3e[3]) & _PAGE_PRESENT);
            ml2e = hap_map_domain_page(_mfn(l3e_get_pfn(ml3e[3])));
            ml2e += l2_table_offset(RO_MPT_VIRT_START);
            hap_unmap_domain_page(ml3e);
        }
        ml2e[index] = l2e_from_pfn(l3e_get_pfn(*l3e), __PAGE_HYPERVISOR);
        if ( v != current )
            hap_unmap_domain_page(ml2e);
    }
}
#endif
#endif  /* __UXEN__ */

#ifndef __UXEN__
static void
hap_write_p2m_entry(struct vcpu *v, unsigned long gfn, l1_pgentry_t *p,
                    l1_pgentry_t new, unsigned int level)
{
    struct domain *d = v->domain;
    uint32_t old_flags;
    int ax_tlbflush = 0;
#ifndef __UXEN__
    bool_t flush_nestedp2m = 0;
#endif  /* __UXEN__ */

    /* We know always use the host p2m here, regardless if the vcpu
     * is in host or guest mode. The vcpu can be in guest mode by
     * a hypercall which passes a domain and chooses mostly the first
     * vcpu. */

    paging_lock(d);
    old_flags = l1e_get_flags(*p);

#ifndef __UXEN__
    if ( nestedhvm_enabled(d) && (old_flags & _PAGE_PRESENT) 
         && !p2m_get_hostp2m(d)->defer_nested_flush ) {
        /* We are replacing a valid entry so we need to flush nested p2ms,
         * unless the only change is an increase in access rights. */
        mfn_t omfn = _mfn(l1e_get_pfn(*p));
        mfn_t nmfn = _mfn(l1e_get_pfn(new));
        flush_nestedp2m = !( mfn_x(omfn) == mfn_x(nmfn)
            && perms_strictly_increased(old_flags, l1e_get_flags(new)) );
    }
#endif  /* __UXEN__ */

    safe_write_pte(p, new);
    if ( (old_flags & _PAGE_PRESENT)
         && !p2m_is_logdirty(p2m_flags_to_type(l1e_get_flags(new)))
         && (level == 1 || (level == 2 && (old_flags & _PAGE_PSE))) ) {
             if (!ax_pv_ept)
                pt_sync_domain(d);
             else
                ax_tlbflush = 1;
    }

#ifndef __UXEN__
#if CONFIG_PAGING_LEVELS == 3
    /* install P2M in monitor table for PAE Xen */
    if ( level == 3 )
        /* We have written to the p2m l3: need to sync the per-vcpu
         * copies of it in the monitor tables */
        p2m_install_entry_in_monitors(d, (l3_pgentry_t *)p);
#endif
#endif  /* __UXEN__ */

    if (ax_pv_ept && (boot_cpu_data.x86_vendor == X86_VENDOR_AMD)) {
        struct p2m_domain *p2m = p2m_get_hostp2m(v->domain);

        ax_pv_ept_write(p2m, level - 1, gfn, *((uint64_t *) &new), ax_tlbflush);
    }

    paging_unlock(d);

#ifndef __UXEN__
    if ( flush_nestedp2m )
        p2m_flush_nestedp2m(d);
#endif  /* __UXEN__ */
}
#endif  /* __UXEN__ */

static unsigned long hap_gva_to_gfn_real_mode(
    struct vcpu *v, struct p2m_domain *p2m, unsigned long gva,
    paging_g2g_query_t q, uint32_t *pfec)
{
    return ((paddr_t)gva >> PAGE_SHIFT);
}

static unsigned long hap_p2m_ga_to_gfn_real_mode(
    struct vcpu *v, struct p2m_domain *p2m, unsigned long cr3,
    paddr_t ga, paging_g2g_query_t q, uint32_t *pfec, unsigned int *page_order)
{
DEBUG();
    if ( page_order )
        *page_order = PAGE_ORDER_4K;
    return (ga >> PAGE_SHIFT);
}

/* Entry points into this mode of the hap code. */
static const struct paging_mode hap_paging_real_mode = {
    .page_fault             = hap_page_fault,
    .invlpg                 = hap_invlpg,
    .gva_to_gfn             = hap_gva_to_gfn_real_mode,
    .p2m_ga_to_gfn          = hap_p2m_ga_to_gfn_real_mode,
    .update_cr3             = hap_update_cr3,
    .update_paging_modes    = hap_update_paging_modes,
#ifndef __UXEN__
    .write_p2m_entry        = hap_write_p2m_entry,
#endif  /* __UXEN__ */
    .guest_levels           = 1
};

static const struct paging_mode hap_paging_protected_mode = {
    .page_fault             = hap_page_fault,
    .invlpg                 = hap_invlpg,
    .gva_to_gfn             = hap_gva_to_gfn_2_levels,
    .p2m_ga_to_gfn          = hap_p2m_ga_to_gfn_2_levels,
    .update_cr3             = hap_update_cr3,
    .update_paging_modes    = hap_update_paging_modes,
#ifndef __UXEN__
    .write_p2m_entry        = hap_write_p2m_entry,
#endif  /* __UXEN__ */
    .guest_levels           = 2
};

static const struct paging_mode hap_paging_pae_mode = {
    .page_fault             = hap_page_fault,
    .invlpg                 = hap_invlpg,
    .gva_to_gfn             = hap_gva_to_gfn_3_levels,
    .p2m_ga_to_gfn          = hap_p2m_ga_to_gfn_3_levels,
    .update_cr3             = hap_update_cr3,
    .update_paging_modes    = hap_update_paging_modes,
#ifndef __UXEN__
    .write_p2m_entry        = hap_write_p2m_entry,
#endif  /* __UXEN__ */
    .guest_levels           = 3
};

#if CONFIG_PAGING_LEVELS == 4
static const struct paging_mode hap_paging_long_mode = {
    .page_fault             = hap_page_fault,
    .invlpg                 = hap_invlpg,
    .gva_to_gfn             = hap_gva_to_gfn_4_levels,
    .p2m_ga_to_gfn          = hap_p2m_ga_to_gfn_4_levels,
    .update_cr3             = hap_update_cr3,
    .update_paging_modes    = hap_update_paging_modes,
#ifndef __UXEN__
    .write_p2m_entry        = hap_write_p2m_entry,
#endif  /* __UXEN__ */
    .guest_levels           = 4
};
#endif

/*
 * Local variables:
 * mode: C
 * c-set-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
