/******************************************************************************
 * hvm/emulate.c
 * 
 * HVM instruction emulation. Used for MMIO and VMX real mode.
 * 
 * Copyright (c) 2008, Citrix Systems, Inc.
 * 
 * Authors:
 *    Keir Fraser <keir@xen.org>
 */

#include <xen/config.h>
#include <xen/init.h>
#include <xen/lib.h>
#include <xen/sched.h>
#include <xen/paging.h>
#include <xen/trace.h>
#include <asm/event.h>
#include <asm/hvm/emulate.h>
#include <asm/hvm/hvm.h>
#include <asm/hvm/trace.h>
#include <asm/hvm/support.h>

static void hvmtrace_io_assist(int is_mmio, ioreq_t *p)
{
    unsigned int size, event;
    unsigned char buffer[12];

    if ( likely(!tb_init_done) )
        return;

    if ( is_mmio )
        event = p->dir ? TRC_HVM_IOMEM_READ : TRC_HVM_IOMEM_WRITE;
    else
        event = p->dir ? TRC_HVM_IOPORT_READ : TRC_HVM_IOPORT_WRITE;

    *(uint64_t *)buffer = p->addr;
    size = (p->addr != (u32)p->addr) ? 8 : 4;
    if ( size == 8 )
        event |= TRC_64_FLAG;

    if ( !p->data_is_ptr )
    {
        *(uint32_t *)&buffer[size] = p->data;
        size += 4;
    }

    trace_var(event, 0/*!cycles*/, size, buffer);
}

static int
hvmemul_prepare_assist(ioreq_t *p)
{
    struct vcpu *v = current;
    struct hvm_ioreq_server *s;
    int i;
    int sign;
    uint32_t data = ~0;

    if (p->type == IOREQ_TYPE_PCI_CONFIG)
        return X86EMUL_UNHANDLEABLE;

    spin_lock(&v->domain->arch.hvm_domain.ioreq_server_lock);
    for (s = v->domain->arch.hvm_domain.ioreq_server_list; s; s = s->next) {
        struct hvm_io_range *x = (p->type == IOREQ_TYPE_COPY)
            ? s->mmio_range_list : s->portio_range_list;

        /* first server, no other server, and no region list -- use
         * the first server */
        if (s == v->domain->arch.hvm_domain.ioreq_server_list &&
            !s->next && !x) {
            set_ioreq(v, &s->ioreq, p);
            spin_unlock(&v->domain->arch.hvm_domain.ioreq_server_lock);
            return X86EMUL_UNHANDLEABLE;
        }

        for ( ; x; x = x->next) {
            if ((p->addr >= x->s) && (p->addr <= x->e)) {
                set_ioreq(v, &s->ioreq, p);
                spin_unlock(&v->domain->arch.hvm_domain.ioreq_server_lock);
                return X86EMUL_UNHANDLEABLE;
            }
        }
    }
    spin_unlock(&v->domain->arch.hvm_domain.ioreq_server_lock);

    if (p->dir != IOREQ_WRITE) {
        if (!p->data_is_ptr)
            p->data = ~0;
        else {
            sign = p->df ? -1 : 1;

            for (i = 0; i < p->count; i++)
                hvm_copy_to_guest_phys(p->data + sign * i * p->size, &data,
                                       p->size);
        }
    }

    return X86EMUL_OKAY;
}

static int hvmemul_do_io(
    int is_mmio, paddr_t addr, unsigned long *reps, int size,
    paddr_t ram_gpa, int dir, int df, void *p_data)
{
    paddr_t value = ram_gpa;
    int value_is_ptr = (p_data == NULL);
    struct vcpu *curr = current;
    struct hvm_vcpu_io *vio;
    ioreq_t *p = get_ioreq(curr);
    unsigned long ram_gfn = paddr_to_pfn(ram_gpa);
    mfn_t mfn;
    p2m_type_t p2mt;
    int rc;

    /* Check for paged out page */
    mfn = get_gfn_unshare(curr->domain, ram_gfn, &p2mt);
#ifndef __UXEN__
    if ( p2m_is_paging(p2mt) )
    {
        p2m_mem_paging_populate(curr->domain, ram_gfn);
        put_gfn(curr->domain, ram_gfn); 
        return X86EMUL_RETRY;
    }
    if ( p2m_is_shared(p2mt) )
    {
        put_gfn(curr->domain, ram_gfn); 
        return X86EMUL_RETRY;
    }
#else  /* __UXEN__ */
    if (mfn_retry(mfn)) {
        put_gfn(curr->domain, ram_gfn);
        return X86EMUL_RETRY;
    }
#endif  /* __UXEN__ */

    /*
     * Weird-sized accesses have undefined behaviour: we discard writes
     * and read all-ones.
     */
    if ( unlikely((size > sizeof(long)) || (size & (size - 1))) )
    {
        gdprintk(XENLOG_WARNING, "bad mmio size %d\n", size);
        ASSERT(p_data != NULL); /* cannot happen with a REP prefix */
        if ( dir == IOREQ_READ )
            memset(p_data, ~0, size);
        put_gfn(curr->domain, ram_gfn); 
        return X86EMUL_UNHANDLEABLE;
    }

    if ( (p_data != NULL) && (dir == IOREQ_WRITE) )
    {
        value = 0;
        memcpy(&value, p_data, size);
        p_data = NULL;
    }

    vio = &curr->arch.hvm_vcpu.hvm_io;

    if ( is_mmio && !value_is_ptr )
    {
        /* Part of a multi-cycle read or write? */
        if ( dir == IOREQ_WRITE )
        {
            paddr_t pa = vio->mmio_large_write_pa;
            unsigned int bytes = vio->mmio_large_write_bytes;
            if ( (addr >= pa) && ((addr + size) <= (pa + bytes)) )
            {
                put_gfn(curr->domain, ram_gfn); 
                return X86EMUL_OKAY;
            }
        }
        else
        {
            paddr_t pa = vio->mmio_large_read_pa;
            unsigned int bytes = vio->mmio_large_read_bytes;
            if ( (addr >= pa) && ((addr + size) <= (pa + bytes)) )
            {
                memcpy(p_data, &vio->mmio_large_read[addr - pa],
                       size);
                put_gfn(curr->domain, ram_gfn); 
                return X86EMUL_OKAY;
            }
        }
    }

    switch ( vio->io_state )
    {
    case HVMIO_none:
        break;
    case HVMIO_completed:
        vio->io_state = HVMIO_none;
        if ( p_data == NULL )
        {
            put_gfn(curr->domain, ram_gfn);
            return X86EMUL_UNHANDLEABLE;
        }
        goto finish_access;
    case HVMIO_dispatched:
        /* May have to wait for previous cycle of a multi-write to complete. */
        if ( is_mmio && !value_is_ptr && (dir == IOREQ_WRITE) &&
             (addr == (vio->mmio_large_write_pa +
                       vio->mmio_large_write_bytes)) )
        {
            put_gfn(curr->domain, ram_gfn);
            return X86EMUL_RETRY;
        }
    default:
        put_gfn(curr->domain, ram_gfn);
        return X86EMUL_UNHANDLEABLE;
    }

    if ( p->state != STATE_IOREQ_NONE )
    {
        gdprintk(XENLOG_WARNING, "WARNING: io already pending (%d)?\n",
                 p->state);
        put_gfn(curr->domain, ram_gfn); 
        return X86EMUL_UNHANDLEABLE;
    }

    vio->io_state =
        (p_data == NULL) ? HVMIO_dispatched : HVMIO_awaiting_completion;
    vio->io_size = size;

    /* Use the default shared page */
    current->arch.hvm_vcpu.ioreq_page = &curr->domain->arch.hvm_domain.ioreq;

    p = get_ioreq(current);
    p->state = STATE_IOREQ_NONE;
    p->dir = dir;
    p->data_is_ptr = value_is_ptr;
    p->type = is_mmio ? IOREQ_TYPE_COPY : IOREQ_TYPE_PIO;
    p->size = size;
    p->addr = addr;
    p->count = *reps;
    p->df = df;
    p->data = value;

    hvmtrace_io_assist(is_mmio, p);

    if ( is_mmio )
    {
        rc = hvm_mmio_intercept(p);
        if (rc == X86EMUL_UNHANDLEABLE)
            rc = hvm_internal_pci_intercept(p, PCI_TYPE_MMIO);
#ifndef __UXEN__
        if ( rc == X86EMUL_UNHANDLEABLE )
            rc = hvm_buffered_io_intercept(p);
#endif  /* __UXEN__ */
    }
    else
    {
        rc = hvm_portio_intercept(p);
        if (rc == X86EMUL_UNHANDLEABLE)
            rc = hvm_internal_pci_intercept(p, PCI_TYPE_IO);

        /* If this has now turned into a PCI_CONFIG ioreq, go round
         * the handlers again to give any xen pci devices a bite at
         * the cherry */
        if ((rc == X86EMUL_UNHANDLEABLE) && (p->type == IOREQ_TYPE_PCI_CONFIG))
            rc = hvm_pciconfig_intercept(p);
    }

    if (rc == X86EMUL_UNHANDLEABLE)
        rc = hvmemul_prepare_assist(p);

    switch ( rc )
    {
    case X86EMUL_OKAY:
    case X86EMUL_RETRY:
        *reps = p->count;
        p->state = STATE_IORESP_READY;
        hvm_io_assist();
        vio->io_state = HVMIO_none;
        break;
    case X86EMUL_UNHANDLEABLE:
        rc = X86EMUL_RETRY;
        if ( !hvm_send_assist_req(curr) )
            vio->io_state = HVMIO_none;
        else if ( p_data == NULL )
            rc = X86EMUL_OKAY;
        break;
    default:
        BUG();
    }

    if ( rc != X86EMUL_OKAY )
    {
        put_gfn(curr->domain, ram_gfn); 
        return rc;
    }

 finish_access:
    if ( p_data != NULL )
        memcpy(p_data, &vio->io_data, size);

    if ( is_mmio && !value_is_ptr )
    {
        /* Part of a multi-cycle read or write? */
        if ( dir == IOREQ_WRITE )
        {
            paddr_t pa = vio->mmio_large_write_pa;
            unsigned int bytes = vio->mmio_large_write_bytes;
            if ( bytes == 0 )
                pa = vio->mmio_large_write_pa = addr;
            if ( addr == (pa + bytes) )
                vio->mmio_large_write_bytes += size;
        }
        else
        {
            paddr_t pa = vio->mmio_large_read_pa;
            unsigned int bytes = vio->mmio_large_read_bytes;
            if ( bytes == 0 )
                pa = vio->mmio_large_read_pa = addr;
            if ( (addr == (pa + bytes)) &&
                 ((bytes + size) <
                  sizeof(vio->mmio_large_read)) )
            {
                memcpy(&vio->mmio_large_read[addr - pa], p_data, size);
                vio->mmio_large_read_bytes += size;
            }
        }
    }

    put_gfn(curr->domain, ram_gfn); 
    return X86EMUL_OKAY;
}

int hvmemul_do_pio(
    unsigned long port, unsigned long *reps, int size,
    paddr_t ram_gpa, int dir, int df, void *p_data)
{
    return hvmemul_do_io(0, port, reps, size, ram_gpa, dir, df, p_data);
}

static int hvmemul_do_mmio(
    paddr_t gpa, unsigned long *reps, int size,
    paddr_t ram_gpa, int dir, int df, void *p_data)
{
    return hvmemul_do_io(1, gpa, reps, size, ram_gpa, dir, df, p_data);
}

/*
 * Convert addr from linear to physical form, valid over the range
 * [addr, addr + *reps * bytes_per_rep]. *reps is adjusted according to
 * the valid computed range. It is always >0 when X86EMUL_OKAY is returned.
 * @pfec indicates the access checks to be performed during page-table walks.
 */
static int hvmemul_linear_to_phys(
    unsigned long addr,
    paddr_t *paddr,
    unsigned int bytes_per_rep,
    unsigned long *reps,
    uint32_t pfec,
    struct hvm_emulate_ctxt *hvmemul_ctxt)
{
    struct vcpu *curr = current;
    unsigned long pfn, npfn, done, todo, i, offset = addr & ~PAGE_MASK;
    int reverse;

    /*
     * Clip repetitions to a sensible maximum. This avoids extensive looping in
     * this function while still amortising the cost of I/O trap-and-emulate.
     */
    *reps = min_t(unsigned long, *reps, 4096);

    /* With no paging it's easy: linear == physical. */
    if ( !(curr->arch.hvm_vcpu.guest_cr[0] & X86_CR0_PG) )
    {
        *paddr = addr;
        return X86EMUL_OKAY;
    }

    /* Reverse mode if this is a backwards multi-iteration string operation. */
    reverse = (hvmemul_ctxt->ctxt.regs->eflags & X86_EFLAGS_DF) && (*reps > 1);

    if ( reverse && ((PAGE_SIZE - offset) < bytes_per_rep) )
    {
        /* Do page-straddling first iteration forwards via recursion. */
        paddr_t _paddr;
        unsigned long one_rep = 1;
        int rc = hvmemul_linear_to_phys(
            addr, &_paddr, bytes_per_rep, &one_rep, pfec, hvmemul_ctxt);
        if ( rc != X86EMUL_OKAY )
            return rc;
        pfn = _paddr >> PAGE_SHIFT;
    }
    else if ( (pfn = paging_gva_to_gfn(curr, addr, paging_g2g_unshare,
                                       &pfec)) == INVALID_GFN )
    {
        if ( pfec == PFEC_page_paged || pfec == PFEC_page_shared )
            return X86EMUL_RETRY;
        hvm_inject_exception(TRAP_page_fault, pfec, addr);
        return X86EMUL_EXCEPTION;
    }

    done = reverse ? bytes_per_rep + offset : PAGE_SIZE - offset;
    todo = *reps * bytes_per_rep;
    for ( i = 1; done < todo; i++ )
    {
        /* Get the next PFN in the range. */
        addr += reverse ? -PAGE_SIZE : PAGE_SIZE;
        npfn = paging_gva_to_gfn(curr, addr, paging_g2g_unshare, &pfec);

        /* Is it contiguous with the preceding PFNs? If not then we're done. */
        if ( (npfn == INVALID_GFN) || (npfn != (pfn + (reverse ? -i : i))) )
        {
            if ( pfec == PFEC_page_paged || pfec == PFEC_page_shared )
                return X86EMUL_RETRY;
            done /= bytes_per_rep;
            if ( done == 0 )
            {
                ASSERT(!reverse);
                if ( npfn != INVALID_GFN )
                    return X86EMUL_UNHANDLEABLE;
                hvm_inject_exception(TRAP_page_fault, pfec, addr & PAGE_MASK);
                return X86EMUL_EXCEPTION;
            }
            *reps = done;
            break;
        }

        done += PAGE_SIZE;
    }

    *paddr = ((paddr_t)pfn << PAGE_SHIFT) | offset;
    return X86EMUL_OKAY;
}
    

static int hvmemul_virtual_to_linear(
    enum x86_segment seg,
    unsigned long offset,
    unsigned int bytes_per_rep,
    unsigned long *reps,
    enum hvm_access_type access_type,
    struct hvm_emulate_ctxt *hvmemul_ctxt,
    unsigned long *paddr)
{
    struct segment_register *reg;
    int okay;

    if ( seg == x86_seg_none )
    {
        *paddr = offset;
        return X86EMUL_OKAY;
    }

    /*
     * Clip repetitions to avoid overflow when multiplying by @bytes_per_rep.
     * The chosen maximum is very conservative but it's what we use in
     * hvmemul_linear_to_phys() so there is no point in using a larger value.
     */
    *reps = min_t(unsigned long, *reps, 4096);

    reg = hvmemul_get_seg_reg(seg, hvmemul_ctxt);

    if ( (hvmemul_ctxt->ctxt.regs->eflags & X86_EFLAGS_DF) && (*reps > 1) )
    {
        /*
         * x86_emulate() clips the repetition count to ensure we don't wrap
         * the effective-address index register. Hence this assertion holds.
         */
        ASSERT(offset >= ((*reps - 1) * bytes_per_rep));
        okay = hvm_virtual_to_linear_addr(
            seg, reg, offset - (*reps - 1) * bytes_per_rep,
            *reps * bytes_per_rep, access_type,
            hvmemul_ctxt->ctxt.addr_size, paddr);
        *paddr += (*reps - 1) * bytes_per_rep;
        if ( hvmemul_ctxt->ctxt.addr_size != 64 )
            *paddr = (uint32_t)*paddr;
    }
    else
    {
        okay = hvm_virtual_to_linear_addr(
            seg, reg, offset, *reps * bytes_per_rep, access_type,
            hvmemul_ctxt->ctxt.addr_size, paddr);
    }

    if ( okay )
        return X86EMUL_OKAY;

    /* If this is a string operation, emulate each iteration separately. */
    if ( *reps != 1 )
        return X86EMUL_UNHANDLEABLE;

    /* This is a singleton operation: fail it with an exception. */
    hvmemul_ctxt->exn_pending = 1;
    hvmemul_ctxt->exn_vector = TRAP_gp_fault;
    hvmemul_ctxt->exn_error_code = 0;
    hvmemul_ctxt->exn_insn_len = 0;
    return X86EMUL_EXCEPTION;
}

static int __hvmemul_read(
    enum x86_segment seg,
    unsigned long offset,
    void *p_data,
    unsigned int bytes,
    enum hvm_access_type access_type,
    struct hvm_emulate_ctxt *hvmemul_ctxt)
{
    struct vcpu *curr = current;
    unsigned long addr, reps = 1;
    uint32_t pfec = PFEC_page_present;
    struct hvm_vcpu_io *vio = &curr->arch.hvm_vcpu.hvm_io;
    paddr_t gpa;
    int rc;

    rc = hvmemul_virtual_to_linear(
        seg, offset, bytes, &reps, access_type, hvmemul_ctxt, &addr);
    if ( rc != X86EMUL_OKAY )
        return rc;

    if ( unlikely(vio->mmio_gva == (addr & PAGE_MASK)) && vio->mmio_gva )
    {
        unsigned int off = addr & (PAGE_SIZE - 1);
        if ( access_type == hvm_access_insn_fetch )
            return X86EMUL_UNHANDLEABLE;
        gpa = (((paddr_t)vio->mmio_gpfn << PAGE_SHIFT) | off);
        if ( (off + bytes) <= PAGE_SIZE )
            return hvmemul_do_mmio(gpa, &reps, bytes, 0,
                                   IOREQ_READ, 0, p_data);
    }

    if ( (seg != x86_seg_none) &&
         (hvmemul_ctxt->seg_reg[x86_seg_ss].attr.fields.dpl == 3) )
        pfec |= PFEC_user_mode;

    rc = ((access_type == hvm_access_insn_fetch) ?
          hvm_fetch_from_guest_virt(p_data, addr, bytes, pfec) :
          hvm_copy_from_guest_virt(p_data, addr, bytes, pfec));

    switch ( rc )
    {
    case HVMCOPY_bad_gva_to_gfn:
        return X86EMUL_EXCEPTION;
    case HVMCOPY_unhandleable:
        return X86EMUL_UNHANDLEABLE;
    case  HVMCOPY_bad_gfn_to_mfn:
        if ( access_type == hvm_access_insn_fetch )
            return X86EMUL_UNHANDLEABLE;
        rc = hvmemul_linear_to_phys(
            addr, &gpa, bytes, &reps, pfec, hvmemul_ctxt);
        if ( rc != X86EMUL_OKAY )
            return rc;
        return hvmemul_do_mmio(gpa, &reps, bytes, 0, IOREQ_READ, 0, p_data);
    case HVMCOPY_gfn_paged_out:
        return X86EMUL_RETRY;
    case HVMCOPY_gfn_shared:
        return X86EMUL_RETRY;
    default:
        break;
    }

    return X86EMUL_OKAY;
}

static int hvmemul_read(
    enum x86_segment seg,
    unsigned long offset,
    void *p_data,
    unsigned int bytes,
    struct x86_emulate_ctxt *ctxt)
{
    return __hvmemul_read(
        seg, offset, p_data, bytes, hvm_access_read,
        container_of(ctxt, struct hvm_emulate_ctxt, ctxt));
}

static int hvmemul_insn_fetch(
    enum x86_segment seg,
    unsigned long offset,
    void *p_data,
    unsigned int bytes,
    struct x86_emulate_ctxt *ctxt)
{
    struct hvm_emulate_ctxt *hvmemul_ctxt =
        container_of(ctxt, struct hvm_emulate_ctxt, ctxt);
    unsigned int insn_off = offset - hvmemul_ctxt->insn_buf_eip;

    /* Fall back if requested bytes are not in the prefetch cache. */
    if ( unlikely((insn_off + bytes) > hvmemul_ctxt->insn_buf_bytes) )
        return __hvmemul_read(
            seg, offset, p_data, bytes,
            hvm_access_insn_fetch, hvmemul_ctxt);

    /* Hit the cache. Simple memcpy. */
    memcpy(p_data, &hvmemul_ctxt->insn_buf[insn_off], bytes);
    return X86EMUL_OKAY;
}

static int hvmemul_write(
    enum x86_segment seg,
    unsigned long offset,
    void *p_data,
    unsigned int bytes,
    struct x86_emulate_ctxt *ctxt)
{
    struct hvm_emulate_ctxt *hvmemul_ctxt =
        container_of(ctxt, struct hvm_emulate_ctxt, ctxt);
    struct vcpu *curr = current;
    unsigned long addr, reps = 1;
    uint32_t pfec = PFEC_page_present | PFEC_write_access;
    struct hvm_vcpu_io *vio = &curr->arch.hvm_vcpu.hvm_io;
    paddr_t gpa;
    int rc;

    rc = hvmemul_virtual_to_linear(
        seg, offset, bytes, &reps, hvm_access_write, hvmemul_ctxt, &addr);
    if ( rc != X86EMUL_OKAY )
        return rc;

    if ( unlikely(vio->mmio_gva == (addr & PAGE_MASK)) && vio->mmio_gva )
    {
        unsigned int off = addr & (PAGE_SIZE - 1);
        gpa = (((paddr_t)vio->mmio_gpfn << PAGE_SHIFT) | off);
        if ( (off + bytes) <= PAGE_SIZE )
            return hvmemul_do_mmio(gpa, &reps, bytes, 0,
                                   IOREQ_WRITE, 0, p_data);
    }

    if ( (seg != x86_seg_none) &&
         (hvmemul_ctxt->seg_reg[x86_seg_ss].attr.fields.dpl == 3) )
        pfec |= PFEC_user_mode;

    rc = hvm_copy_to_guest_virt(addr, p_data, bytes, pfec);

    switch ( rc )
    {
    case HVMCOPY_bad_gva_to_gfn:
        return X86EMUL_EXCEPTION;
    case HVMCOPY_unhandleable:
        return X86EMUL_UNHANDLEABLE;
    case HVMCOPY_bad_gfn_to_mfn:
        rc = hvmemul_linear_to_phys(
            addr, &gpa, bytes, &reps, pfec, hvmemul_ctxt);
        if ( rc != X86EMUL_OKAY )
            return rc;
        return hvmemul_do_mmio(gpa, &reps, bytes, 0,
                               IOREQ_WRITE, 0, p_data);
    case HVMCOPY_gfn_paged_out:
        return X86EMUL_RETRY;
    case HVMCOPY_gfn_shared:
        return X86EMUL_RETRY;
    default:
        break;
    }

    return X86EMUL_OKAY;
}

static int hvmemul_cmpxchg(
    enum x86_segment seg,
    unsigned long offset,
    void *p_old,
    void *p_new,
    unsigned int bytes,
    struct x86_emulate_ctxt *ctxt)
{
    /* Fix this in case the guest is really relying on r-m-w atomicity. */
    return hvmemul_write(seg, offset, p_new, bytes, ctxt);
}

static int hvmemul_rep_ins(
    uint16_t src_port,
    enum x86_segment dst_seg,
    unsigned long dst_offset,
    unsigned int bytes_per_rep,
    unsigned long *reps,
    struct x86_emulate_ctxt *ctxt)
{
    struct hvm_emulate_ctxt *hvmemul_ctxt =
        container_of(ctxt, struct hvm_emulate_ctxt, ctxt);
    unsigned long addr;
    uint32_t pfec = PFEC_page_present | PFEC_write_access;
    paddr_t gpa;
    int rc;

    rc = hvmemul_virtual_to_linear(
        dst_seg, dst_offset, bytes_per_rep, reps, hvm_access_write,
        hvmemul_ctxt, &addr);
    if ( rc != X86EMUL_OKAY )
        return rc;

    if ( hvmemul_ctxt->seg_reg[x86_seg_ss].attr.fields.dpl == 3 )
        pfec |= PFEC_user_mode;

    rc = hvmemul_linear_to_phys(
        addr, &gpa, bytes_per_rep, reps, pfec, hvmemul_ctxt);
    if ( rc != X86EMUL_OKAY )
        return rc;

    return hvmemul_do_pio(src_port, reps, bytes_per_rep, gpa, IOREQ_READ,
                          !!(ctxt->regs->eflags & X86_EFLAGS_DF), NULL);
}

static int hvmemul_rep_outs(
    enum x86_segment src_seg,
    unsigned long src_offset,
    uint16_t dst_port,
    unsigned int bytes_per_rep,
    unsigned long *reps,
    struct x86_emulate_ctxt *ctxt)
{
    struct hvm_emulate_ctxt *hvmemul_ctxt =
        container_of(ctxt, struct hvm_emulate_ctxt, ctxt);
    unsigned long addr;
    uint32_t pfec = PFEC_page_present;
    paddr_t gpa;
    int rc;

    rc = hvmemul_virtual_to_linear(
        src_seg, src_offset, bytes_per_rep, reps, hvm_access_read,
        hvmemul_ctxt, &addr);
    if ( rc != X86EMUL_OKAY )
        return rc;

    if ( hvmemul_ctxt->seg_reg[x86_seg_ss].attr.fields.dpl == 3 )
        pfec |= PFEC_user_mode;

    rc = hvmemul_linear_to_phys(
        addr, &gpa, bytes_per_rep, reps, pfec, hvmemul_ctxt);
    if ( rc != X86EMUL_OKAY )
        return rc;

    return hvmemul_do_pio(dst_port, reps, bytes_per_rep, gpa, IOREQ_WRITE,
                          !!(ctxt->regs->eflags & X86_EFLAGS_DF), NULL);
}

static int hvmemul_rep_movs(
   enum x86_segment src_seg,
   unsigned long src_offset,
   enum x86_segment dst_seg,
   unsigned long dst_offset,
   unsigned int bytes_per_rep,
   unsigned long *reps,
   struct x86_emulate_ctxt *ctxt)
{
    struct hvm_emulate_ctxt *hvmemul_ctxt =
        container_of(ctxt, struct hvm_emulate_ctxt, ctxt);
    unsigned long saddr, daddr, bytes;
    paddr_t sgpa, dgpa;
    uint32_t pfec = PFEC_page_present;
    p2m_type_t p2mt;
    int rc, df = !!(ctxt->regs->eflags & X86_EFLAGS_DF);
    char *buf;
    mfn_t mfn;

    rc = hvmemul_virtual_to_linear(
        src_seg, src_offset, bytes_per_rep, reps, hvm_access_read,
        hvmemul_ctxt, &saddr);
    if ( rc != X86EMUL_OKAY )
        return rc;

    rc = hvmemul_virtual_to_linear(
        dst_seg, dst_offset, bytes_per_rep, reps, hvm_access_write,
        hvmemul_ctxt, &daddr);
    if ( rc != X86EMUL_OKAY )
        return rc;

    if ( hvmemul_ctxt->seg_reg[x86_seg_ss].attr.fields.dpl == 3 )
        pfec |= PFEC_user_mode;

    rc = hvmemul_linear_to_phys(
        saddr, &sgpa, bytes_per_rep, reps, pfec, hvmemul_ctxt);
    if ( rc != X86EMUL_OKAY )
        return rc;

    rc = hvmemul_linear_to_phys(
        daddr, &dgpa, bytes_per_rep, reps,
        pfec | PFEC_write_access, hvmemul_ctxt);
    if ( rc != X86EMUL_OKAY )
        return rc;

    /* Unlocked works here because we get_gfn for real in whatever
     * we call later. */
    mfn = get_gfn_unlocked(current->domain, sgpa >> PAGE_SHIFT, &p2mt);
    if (mfn_retry(mfn))
        return X86EMUL_RETRY;
    if (!p2m_is_ram(p2mt)
#ifndef __UXEN__
        && !p2m_is_grant(p2mt)
#endif  /* __UXEN__ */
        )
        return hvmemul_do_mmio(
            sgpa, reps, bytes_per_rep, dgpa, IOREQ_READ, df, NULL);

    mfn = get_gfn_unlocked(current->domain, dgpa >> PAGE_SHIFT, &p2mt);
    if (mfn_retry(mfn))
        return X86EMUL_RETRY;
    if (!p2m_is_ram(p2mt)
#ifndef __UXEN__
        && !p2m_is_grant(p2mt)
#endif  /* __UXEN__ */
        )
        return hvmemul_do_mmio(
            dgpa, reps, bytes_per_rep, sgpa, IOREQ_WRITE, df, NULL);

    /* RAM-to-RAM copy: emulate as equivalent of memmove(dgpa, sgpa, bytes). */
    bytes = *reps * bytes_per_rep;

    /* Adjust source address for reverse copy. */
    if ( df )
        sgpa -= bytes - bytes_per_rep;

    /*
     * Will first iteration copy fall within source range? If not then entire
     * copy does not corrupt itself. If so, then this is more complex than
     * can be emulated by a source-to-buffer-to-destination block copy.
     */
    if ( ((dgpa + bytes_per_rep) > sgpa) && (dgpa < (sgpa + bytes)) )
        return X86EMUL_UNHANDLEABLE;

    /* Adjust destination address for reverse copy. */
    if ( df )
        dgpa -= bytes - bytes_per_rep;

    /* Allocate temporary buffer. Fall back to slow emulation if this fails. */
    buf = xmalloc_bytes(bytes);
    if ( buf == NULL )
        return X86EMUL_UNHANDLEABLE;

    /*
     * We do a modicum of checking here, just for paranoia's sake and to
     * definitely avoid copying an unitialised buffer into guest address space.
     */
    rc = hvm_copy_from_guest_phys(buf, sgpa, bytes);
    if ( rc == HVMCOPY_okay )
        rc = hvm_copy_to_guest_phys(dgpa, buf, bytes);

    xfree(buf);

    if ( rc == HVMCOPY_gfn_paged_out )
        return X86EMUL_RETRY;
    if ( rc == HVMCOPY_gfn_shared )
        return X86EMUL_RETRY;
    if ( rc != HVMCOPY_okay )
    {
        gdprintk(XENLOG_WARNING, "Failed memory-to-memory REP MOVS: sgpa=%"
                 PRIpaddr" dgpa=%"PRIpaddr" reps=%lu bytes_per_rep=%u\n",
                 sgpa, dgpa, *reps, bytes_per_rep);
        return X86EMUL_UNHANDLEABLE;
    }

    return X86EMUL_OKAY;
}

static int hvmemul_read_segment(
    enum x86_segment seg,
    struct segment_register *reg,
    struct x86_emulate_ctxt *ctxt)
{
    struct hvm_emulate_ctxt *hvmemul_ctxt =
        container_of(ctxt, struct hvm_emulate_ctxt, ctxt);
    struct segment_register *sreg = hvmemul_get_seg_reg(seg, hvmemul_ctxt);
    memcpy(reg, sreg, sizeof(struct segment_register));
    return X86EMUL_OKAY;
}

static int hvmemul_write_segment(
    enum x86_segment seg,
    struct segment_register *reg,
    struct x86_emulate_ctxt *ctxt)
{
    struct hvm_emulate_ctxt *hvmemul_ctxt =
        container_of(ctxt, struct hvm_emulate_ctxt, ctxt);
    struct segment_register *sreg = hvmemul_get_seg_reg(seg, hvmemul_ctxt);

    memcpy(sreg, reg, sizeof(struct segment_register));
    __set_bit(seg, &hvmemul_ctxt->seg_reg_dirty);

    return X86EMUL_OKAY;
}

static int hvmemul_read_io(
    unsigned int port,
    unsigned int bytes,
    unsigned long *val,
    struct x86_emulate_ctxt *ctxt)
{
    unsigned long reps = 1;
    *val = 0;
    return hvmemul_do_pio(port, &reps, bytes, 0, IOREQ_READ, 0, val);
}

static int hvmemul_write_io(
    unsigned int port,
    unsigned int bytes,
    unsigned long val,
    struct x86_emulate_ctxt *ctxt)
{
    unsigned long reps = 1;
    return hvmemul_do_pio(port, &reps, bytes, 0, IOREQ_WRITE, 0, &val);
}

static int hvmemul_read_cr(
    unsigned int reg,
    unsigned long *val,
    struct x86_emulate_ctxt *ctxt)
{
    switch ( reg )
    {
    case 0:
    case 2:
    case 3:
    case 4:
        *val = current->arch.hvm_vcpu.guest_cr[reg];
        HVMTRACE_LONG_2D(CR_READ, reg, TRC_PAR_LONG(*val));
        return X86EMUL_OKAY;
    default:
        break;
    }

    return X86EMUL_UNHANDLEABLE;
}

static int hvmemul_write_cr(
    unsigned int reg,
    unsigned long val,
    struct x86_emulate_ctxt *ctxt)
{
    HVMTRACE_LONG_2D(CR_WRITE, reg, TRC_PAR_LONG(val));
    switch ( reg )
    {
    case 0:
        return hvm_set_cr0(val);
    case 2:
        current->arch.hvm_vcpu.guest_cr[2] = val;
        return X86EMUL_OKAY;
    case 3:
        return hvm_set_cr3(val);
    case 4:
        return hvm_set_cr4(val);
    default:
        break;
    }

    return X86EMUL_UNHANDLEABLE;
}

static int hvmemul_read_msr(
    unsigned long reg,
    uint64_t *val,
    struct x86_emulate_ctxt *ctxt)
{
    return hvm_msr_read_intercept(reg, val);
}

static int hvmemul_write_msr(
    unsigned long reg,
    uint64_t val,
    struct x86_emulate_ctxt *ctxt)
{
    return hvm_msr_write_intercept(reg, val);
}

static int hvmemul_wbinvd(
    struct x86_emulate_ctxt *ctxt)
{
    HVM_FUNCS(wbinvd_intercept);
    return X86EMUL_OKAY;
}

static int hvmemul_cpuid(
    unsigned int *eax,
    unsigned int *ebx,
    unsigned int *ecx,
    unsigned int *edx,
    struct x86_emulate_ctxt *ctxt)
{
    HVM_FUNCS(cpuid_intercept, eax, ebx, ecx, edx);
    return X86EMUL_OKAY;
}

static int hvmemul_inject_hw_exception(
    uint8_t vector,
    int32_t error_code,
    struct x86_emulate_ctxt *ctxt)
{
    struct hvm_emulate_ctxt *hvmemul_ctxt =
        container_of(ctxt, struct hvm_emulate_ctxt, ctxt);

    hvmemul_ctxt->exn_pending = 1;
    hvmemul_ctxt->exn_vector = vector;
    hvmemul_ctxt->exn_error_code = error_code;
    hvmemul_ctxt->exn_insn_len = 0;

    return X86EMUL_OKAY;
}

static int hvmemul_inject_sw_interrupt(
    uint8_t vector,
    uint8_t insn_len,
    struct x86_emulate_ctxt *ctxt)
{
    struct hvm_emulate_ctxt *hvmemul_ctxt =
        container_of(ctxt, struct hvm_emulate_ctxt, ctxt);

    hvmemul_ctxt->exn_pending = 1;
    hvmemul_ctxt->exn_vector = vector;
    hvmemul_ctxt->exn_error_code = -1;
    hvmemul_ctxt->exn_insn_len = insn_len;

    return X86EMUL_OKAY;
}

static int hvmemul_get_fpu(
    void (*exception_callback)(void *, struct cpu_user_regs *),
    void *exception_callback_arg,
    enum x86_emulate_fpu_type type,
    struct x86_emulate_ctxt *ctxt)
{
    struct vcpu *curr = current;

    switch ( type )
    {
    case X86EMUL_FPU_fpu:
        break;
    case X86EMUL_FPU_mmx:
        if ( !cpu_has_mmx )
            return X86EMUL_UNHANDLEABLE;
        break;
    default:
        return X86EMUL_UNHANDLEABLE;
    }

    if ( !curr->fpu_dirtied )
        HVM_FUNCS(fpu_dirty_intercept);

    curr->arch.hvm_vcpu.fpu_exception_callback = exception_callback;
    curr->arch.hvm_vcpu.fpu_exception_callback_arg = exception_callback_arg;

    return X86EMUL_OKAY;
}

static void hvmemul_put_fpu(
    struct x86_emulate_ctxt *ctxt)
{
    struct vcpu *curr = current;
    curr->arch.hvm_vcpu.fpu_exception_callback = NULL;
}

static int hvmemul_invlpg(
    enum x86_segment seg,
    unsigned long offset,
    struct x86_emulate_ctxt *ctxt)
{
    struct hvm_emulate_ctxt *hvmemul_ctxt =
        container_of(ctxt, struct hvm_emulate_ctxt, ctxt);
    unsigned long addr, reps = 1;
    int rc;

    rc = hvmemul_virtual_to_linear(
        seg, offset, 1, &reps, hvm_access_none, hvmemul_ctxt, &addr);

    if ( rc == X86EMUL_OKAY )
        HVM_FUNCS(invlpg_intercept, addr);

    return rc;
}

static const struct x86_emulate_ops hvm_emulate_ops = {
    .read          = hvmemul_read,
    .insn_fetch    = hvmemul_insn_fetch,
    .write         = hvmemul_write,
    .cmpxchg       = hvmemul_cmpxchg,
    .rep_ins       = hvmemul_rep_ins,
    .rep_outs      = hvmemul_rep_outs,
    .rep_movs      = hvmemul_rep_movs,
    .read_segment  = hvmemul_read_segment,
    .write_segment = hvmemul_write_segment,
    .read_io       = hvmemul_read_io,
    .write_io      = hvmemul_write_io,
    .read_cr       = hvmemul_read_cr,
    .write_cr      = hvmemul_write_cr,
    .read_msr      = hvmemul_read_msr,
    .write_msr     = hvmemul_write_msr,
    .wbinvd        = hvmemul_wbinvd,
    .cpuid         = hvmemul_cpuid,
    .inject_hw_exception = hvmemul_inject_hw_exception,
    .inject_sw_interrupt = hvmemul_inject_sw_interrupt,
    .get_fpu       = hvmemul_get_fpu,
    .put_fpu       = hvmemul_put_fpu,
    .invlpg        = hvmemul_invlpg
};

int hvm_emulate_one(
    struct hvm_emulate_ctxt *hvmemul_ctxt)
{
    struct cpu_user_regs *regs = hvmemul_ctxt->ctxt.regs;
    struct vcpu *curr = current;
    uint32_t new_intr_shadow, pfec = PFEC_page_present;
    struct hvm_vcpu_io *vio = &curr->arch.hvm_vcpu.hvm_io;
    unsigned long addr;
    int rc;

    hvmemul_ctxt->ctxt.emulation_restricted =
        curr->domain->arch.hvm_domain.params[HVM_PARAM_RESTRICTED_X86_EMUL];
    hvmemul_ctxt->ctxt.silent_fake_emulation = curr->domain->silent_fake_emulation;

    if ( hvm_long_mode_enabled(curr) &&
         hvmemul_ctxt->seg_reg[x86_seg_cs].attr.fields.l )
    {
        hvmemul_ctxt->ctxt.addr_size = hvmemul_ctxt->ctxt.sp_size = 64;
    }
    else
    {
        hvmemul_ctxt->ctxt.addr_size =
            hvmemul_ctxt->seg_reg[x86_seg_cs].attr.fields.db ? 32 : 16;
        hvmemul_ctxt->ctxt.sp_size =
            hvmemul_ctxt->seg_reg[x86_seg_ss].attr.fields.db ? 32 : 16;
    }

    if ( hvmemul_ctxt->seg_reg[x86_seg_ss].attr.fields.dpl == 3 )
        pfec |= PFEC_user_mode;

    hvmemul_ctxt->insn_buf_eip = regs->eip;
    hvmemul_ctxt->insn_buf_bytes =
        hvm_get_insn_bytes(curr, hvmemul_ctxt->insn_buf)
        ? :
        (hvm_virtual_to_linear_addr(
            x86_seg_cs, &hvmemul_ctxt->seg_reg[x86_seg_cs],
            regs->eip, sizeof(hvmemul_ctxt->insn_buf),
            hvm_access_insn_fetch, hvmemul_ctxt->ctxt.addr_size, &addr) &&
         !hvm_fetch_from_guest_virt_nofault(
             hvmemul_ctxt->insn_buf, addr,
             sizeof(hvmemul_ctxt->insn_buf), pfec))
        ? sizeof(hvmemul_ctxt->insn_buf) : 0;

    hvmemul_ctxt->exn_pending = 0;

    rc = x86_emulate(&hvmemul_ctxt->ctxt, &hvm_emulate_ops);

    if ( rc == X86EMUL_CRASH ) {
        show_execution_state(regs);
        if ( curr->domain->arch.hvm_domain.params[
                 HVM_PARAM_RESTRICTED_X86_EMUL] != 2 ||
             curr->arch.hvm_vcpu.inject_trap != -1 ) {
            __domain_crash(curr->domain);
            return X86EMUL_UNHANDLEABLE;
        } else {
            curr->arch.hvm_vcpu.inject_trap = TRAP_nmi;
            curr->arch.hvm_vcpu.inject_error_code = -1;
            curr->arch.hvm_vcpu.inject_cr2 = 0;
            return X86EMUL_RETRY;
        }
    }

    if ( rc != X86EMUL_RETRY )
        vio->mmio_large_read_bytes = vio->mmio_large_write_bytes = 0;

    if ( rc != X86EMUL_OKAY )
        return rc;

    curr->domain->silent_fake_emulation = hvmemul_ctxt->ctxt.silent_fake_emulation;

    new_intr_shadow = hvmemul_ctxt->intr_shadow;

    /* MOV-SS instruction toggles MOV-SS shadow, else we just clear it. */
    if ( hvmemul_ctxt->ctxt.retire.flags.mov_ss )
        new_intr_shadow ^= HVM_INTR_SHADOW_MOV_SS;
    else
        new_intr_shadow &= ~HVM_INTR_SHADOW_MOV_SS;

    /* STI instruction toggles STI shadow, else we just clear it. */
    if ( hvmemul_ctxt->ctxt.retire.flags.sti )
        new_intr_shadow ^= HVM_INTR_SHADOW_STI;
    else
        new_intr_shadow &= ~HVM_INTR_SHADOW_STI;

    if ( hvmemul_ctxt->intr_shadow != new_intr_shadow )
    {
        hvmemul_ctxt->intr_shadow = new_intr_shadow;
        HVM_FUNCS(set_interrupt_shadow, curr, new_intr_shadow);
    }

    if ( hvmemul_ctxt->ctxt.retire.flags.hlt &&
         !hvm_local_events_need_delivery(curr) )
    {
        hvm_hlt(regs->eflags);
    }

    return X86EMUL_OKAY;
}

void hvm_emulate_prepare(
    struct hvm_emulate_ctxt *hvmemul_ctxt,
    struct cpu_user_regs *regs)
{
    hvmemul_ctxt->intr_shadow = HVM_FUNCS(get_interrupt_shadow, current);
    hvmemul_ctxt->ctxt.regs = regs;
    hvmemul_ctxt->ctxt.force_writeback = 1;
    hvmemul_ctxt->seg_reg_accessed = 0;
    hvmemul_ctxt->seg_reg_dirty = 0;
    hvmemul_get_seg_reg(x86_seg_cs, hvmemul_ctxt);
    hvmemul_get_seg_reg(x86_seg_ss, hvmemul_ctxt);
}

void hvm_emulate_writeback(
    struct hvm_emulate_ctxt *hvmemul_ctxt)
{
    enum x86_segment seg;

    seg = find_first_bit(&hvmemul_ctxt->seg_reg_dirty,
                         ARRAY_SIZE(hvmemul_ctxt->seg_reg));

    while ( seg < ARRAY_SIZE(hvmemul_ctxt->seg_reg) )
    {
        hvm_set_segment_register(current, seg, &hvmemul_ctxt->seg_reg[seg]);
        seg = find_next_bit(&hvmemul_ctxt->seg_reg_dirty,
                            ARRAY_SIZE(hvmemul_ctxt->seg_reg),
                            seg+1);
    }
}

struct segment_register *hvmemul_get_seg_reg(
    enum x86_segment seg,
    struct hvm_emulate_ctxt *hvmemul_ctxt)
{
    if ( !__test_and_set_bit(seg, &hvmemul_ctxt->seg_reg_accessed) )
        hvm_get_segment_register(current, seg, &hvmemul_ctxt->seg_reg[seg]);
    return &hvmemul_ctxt->seg_reg[seg];
}
