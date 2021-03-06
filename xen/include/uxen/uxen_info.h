/*
 *  uxen_info.h
 *  uxen
 *
 *  COPYRIGHT
 * 
 */

#ifndef _UXEN_INFO_H
#define _UXEN_INFO_H

#include "uxen_os.h"
#ifdef __XEN__
#include <public/xen.h>
#else
#include <xen/xen.h>
#endif

#include "uxen_types.h"

/* based on sizeof(ui_cpu_active_mask) */
#define UXEN_MAXIMUM_PROCESSORS (sizeof(uint64_t) * 8)
/* match UXEN_MAXIMUM_PROCESSORS */
#define UXEN_MAXIMUM_VCPUS (sizeof(uint64_t) * 8)

#if defined(__UXEN__) && !defined(__UXEN_SYS__)

/* Minor quantites of magic here */

/* First we mark all the function pointers for the host calls to be deprecated
 * so they generate an error. In UI_HOST_CALL we use magic gcc pragmas to
 * ignore that error */

/* UI_HOST_CALL(a, ...)  calls UI_HOST_CALL_a and then uxen_info->a(...) the
 * complexity in the macro below is to handle the case where the function takes
 * no argument */

/* The various UI_HOST_CALL_a can be defined to UI_HOST_CALL_SAVE_.... as 
 * required */

#include <asm/i387.h>

#if 0
#define __host_call __attribute__                                       \
    (( deprecated("don't call host functions from uxen_info directly, " \
                  "use the UI_HOST_CALL macro") ))
#else
#define __host_call
#endif

#define UI_HOST_CALL_SAVE_XMM                           \
    ({                                                  \
        struct vcpu *v = current;                       \
        if (!uxen_info->host_os_is_xmm_clean && v)      \
            vcpu_save_fpu_hostcall(v);                  \
    }),

#define UI_HOST_CALL_SAVE_NOTHING 0,

#define UI_HOST_CALL_ui_printf                      UI_HOST_CALL_SAVE_XMM
#define UI_HOST_CALL_ui_kick_cpu                    UI_HOST_CALL_SAVE_XMM
#define UI_HOST_CALL_ui_kick_vcpu                   UI_HOST_CALL_SAVE_XMM
#define UI_HOST_CALL_ui_kick_vcpu_cancel            UI_HOST_CALL_SAVE_XMM
#define UI_HOST_CALL_ui_signal_idle_thread          UI_HOST_CALL_SAVE_XMM
#define UI_HOST_CALL_ui_set_timer_vcpu              UI_HOST_CALL_SAVE_XMM
#define UI_HOST_CALL_ui_get_unixtime                UI_HOST_CALL_SAVE_XMM
#define UI_HOST_CALL_ui_get_host_counter            UI_HOST_CALL_SAVE_XMM
#define UI_HOST_CALL_ui_wake_vm                     UI_HOST_CALL_SAVE_XMM
#define UI_HOST_CALL_ui_on_selected_cpus            UI_HOST_CALL_SAVE_XMM
#define UI_HOST_CALL_ui_map_page_global             UI_HOST_CALL_SAVE_XMM
#define UI_HOST_CALL_ui_unmap_page_global_va        UI_HOST_CALL_SAVE_XMM
#define UI_HOST_CALL_ui_map_page                    UI_HOST_CALL_SAVE_XMM
#define UI_HOST_CALL_ui_unmap_page_va               UI_HOST_CALL_SAVE_XMM
#define UI_HOST_CALL_ui_map_page_range              UI_HOST_CALL_SAVE_XMM
#define UI_HOST_CALL_ui_unmap_page_range            UI_HOST_CALL_SAVE_XMM
#define UI_HOST_CALL_ui_mapped_global_va_pfn        UI_HOST_CALL_SAVE_XMM
#define UI_HOST_CALL_ui_host_needs_preempt          UI_HOST_CALL_SAVE_XMM
#define UI_HOST_CALL_ui_notify_exception            UI_HOST_CALL_SAVE_XMM
#define UI_HOST_CALL_ui_notify_vram                 UI_HOST_CALL_SAVE_XMM
#define UI_HOST_CALL_ui_signal_event                UI_HOST_CALL_SAVE_XMM
#define UI_HOST_CALL_ui_check_ioreq                 UI_HOST_CALL_SAVE_XMM
#define UI_HOST_CALL_ui_map_mfn                     UI_HOST_CALL_SAVE_XMM
#define UI_HOST_CALL_ui_user_access_ok              UI_HOST_CALL_SAVE_XMM
#define UI_HOST_CALL_ui_signal_v4v                  UI_HOST_CALL_SAVE_XMM
#define UI_HOST_CALL_ui_rdmsr_safe                  UI_HOST_CALL_SAVE_NOTHING


#define UI_HOST_CALL(...) \
    UI_HOST_CALL_SELECT_WRAP(__VA_ARGS__, UI_HOST_CALLS(__VA_ARGS__)) 

#define UI_HOST_CALL_SELECT_WRAP(...) \
    UI_HOST_CALL_SELECT(__VA_ARGS__) 

#define UI_HOST_CALL_SELECT(B1,B2,B3,B4,B5,B6,B7,B8,B9,B10,B11,B12,B13,B14,B15,B16,B17,B18,B19,N,...) N

#define UI_HOST_CALLS(...)                      \
    UI_HOST_CALL_N(__VA_ARGS__),                \
        UI_HOST_CALL_N(__VA_ARGS__),            \
        UI_HOST_CALL_N(__VA_ARGS__),            \
        UI_HOST_CALL_N(__VA_ARGS__),            \
        UI_HOST_CALL_N(__VA_ARGS__),            \
        UI_HOST_CALL_N(__VA_ARGS__),            \
        UI_HOST_CALL_N(__VA_ARGS__),            \
        UI_HOST_CALL_N(__VA_ARGS__),            \
        UI_HOST_CALL_N(__VA_ARGS__),            \
        UI_HOST_CALL_N(__VA_ARGS__),            \
        UI_HOST_CALL_N(__VA_ARGS__),            \
        UI_HOST_CALL_N(__VA_ARGS__),            \
        UI_HOST_CALL_N(__VA_ARGS__),            \
        UI_HOST_CALL_N(__VA_ARGS__),            \
        UI_HOST_CALL_N(__VA_ARGS__),            \
        UI_HOST_CALL_N(__VA_ARGS__),            \
        UI_HOST_CALL_N(__VA_ARGS__),            \
        UI_HOST_CALL_N(__VA_ARGS__),            \
        UI_HOST_CALL_N(__VA_ARGS__),            \
        UI_HOST_CALL_0(__VA_ARGS__, 0)

#pragma GCC diagnostic error "-Wdeprecated"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated"

#if defined(__x86_64__)
#define UI_HOST_CALL_0(a,b...) \
    (UI_HOST_CALL_ ## a        \
     a())

#define UI_HOST_CALL_N(a,b...) \
    (UI_HOST_CALL_ ## a        \
     a(b))

#else  /* __x86_64__ */
#define UI_HOST_CALL_0(a,b...) \
    (UI_HOST_CALL_ ## a        \
     _ ## a())

#define UI_HOST_CALL_N(a,b...) \
    (UI_HOST_CALL_ ## a        \
     _ ## a(b))

#endif  /* __x86_64__ */

#pragma GCC diagnostic pop

#else
#define __host_call
#endif

#if defined(_MSC_VER)
#define __interface_fn __cdecl __host_call
#if defined(__x86_64__)
#define __interface_fn_fn 
#else
#define __interface_fn_fn /* __stdcall */
#endif
#else /* _MSC_VER */
#if defined(__MS_ABI__)
#if defined(__x86_64__)
#define __interface_fn __attribute__ ((__ms_abi__)) __host_call
#define __interface_fn_fn __attribute__ ((__ms_abi__))
#else
#define __interface_fn __cdecl __host_call
#define __interface_fn_fn __stdcall
#endif
#else /* __MS_ABI__ */
#define __interface_fn __host_call
#define __interface_fn_fn
#endif /* __MS_ABI__ */
#endif /* _MSC_VER */

struct vm_info_shared;
struct vm_vcpu_info_shared;

struct ui_page_list {
    uint32_t list;
    uint32_t count;
};

#if defined(__x86_64__)
#define UI_interface_fn(fn) __interface_fn ui_ ## fn
#else  /* __x86_64__ */
#if !defined(_MSC_VER)
#define UI_interface_fn(fn) __interface_fn _ui_ ## fn
#else  /* _MSC_VER */
#define UI_interface_fn(fn) __interface_fn ui_ ## fn
#endif  /* _MSC_VER */
#endif  /* __x86_64__ */

uint64_t UI_interface_fn(printf)(struct vm_info_shared *,
                                 const char *fmt, ...);
void UI_interface_fn(kick_cpu)(uint64_t, uint64_t);
void UI_interface_fn(kick_vcpu)(struct vm_vcpu_info_shared *);
void UI_interface_fn(kick_vcpu_cancel)(struct vm_vcpu_info_shared *);
void UI_interface_fn(signal_idle_thread)(uint64_t);
void UI_interface_fn(set_timer_vcpu)(struct vm_vcpu_info_shared *, uint64_t);
uint64_t UI_interface_fn(get_unixtime)(void);
uint64_t UI_interface_fn(get_host_counter)(void);
void UI_interface_fn(wake_vm)(struct vm_vcpu_info_shared *);
void UI_interface_fn(on_selected_cpus)(const void *,
                                       __interface_fn_fn
                                       uintptr_t (*)(uintptr_t));
void *UI_interface_fn(map_page_global)(xen_pfn_t);
uint64_t UI_interface_fn(unmap_page_global_va)(const void *);
void *UI_interface_fn(map_page_range)(struct vm_vcpu_info_shared *,
                                      uint64_t, uxen_pfn_t *);
uint64_t UI_interface_fn(unmap_page_range)(
    struct vm_vcpu_info_shared *, const void *, uint64_t, uxen_pfn_t *);
uxen_pfn_t UI_interface_fn(mapped_global_va_pfn)(const void *);
uint64_t UI_interface_fn(host_needs_preempt)(void);
void UI_interface_fn(notify_exception)(struct vm_info_shared *);
void UI_interface_fn(notify_vram)(struct vm_info_shared *);
uint64_t UI_interface_fn(signal_event)(struct vm_vcpu_info_shared *,
                                       void *, void * volatile *);
uint64_t UI_interface_fn(check_ioreq)(struct vm_vcpu_info_shared *);
uint64_t UI_interface_fn(map_mfn)(uintptr_t va, xen_pfn_t mfn);
uint64_t UI_interface_fn(user_access_ok)(void *, void *, uint64_t);
void UI_interface_fn(signal_v4v)(void);
#ifdef UXEN_HOST_OSX
uint64_t UI_interface_fn(rdmsr_safe)(uint32_t, uint64_t *);
#endif  /* UXEN_HOST_OSX */

struct /* __WINPACKED__ */ uxen_info {
    uint32_t ui_running;
    uint32_t ui_sizeof_struct_page_info;
    uint32_t ui_max_page;
    void *ui_frametable;
    uint64_t ui_cpu_active_mask __WINPACKED__;
    uint32_t ui_host_timer_frequency;
    int64_t ui_host_idle_timeout;
    uint32_t ui_unixtime_generation;
    uint32_t ui_exception_event_all;
    uint64_t ui_host_counter;
    uint64_t ui_host_counter_tsc;
    uint64_t ui_host_counter_unixtime;
    uint32_t ui_host_counter_frequency;
    uint32_t ui_map_page_range_max_nr;
    uint32_t ui_pagemap_needs_check;
    char *ui_percpu_area[UXEN_MAXIMUM_PROCESSORS];
    struct ui_page_list ui_free_pages[UXEN_MAXIMUM_PROCESSORS];
#ifdef UXEN_HOST_WINDOWS
    uintptr_t ui_mapcache_va[UXEN_MAXIMUM_PROCESSORS];
    uint32_t ui_mapcache_size;
#endif  /* UXEN_HOST_WINDOWS */
    uint16_t ui_host_gsoff_cpu;
    uint16_t ui_host_gsoff_current;
    uintptr_t *ui_hvm_io_bitmap;
#define UI_HVM_IO_BITMAP_SIZE (3 << PAGE_SHIFT)
    uint32_t ui_vmi_msrpm_size;
    uint32_t ui_xsave_cntxt_size;
    uint32_t ui_domain_array_pages;
    void *ui_domain_array;
#define VFRAMES_PCPU_FILL 2048
    struct ui_page_list ui_vframes;
    uint32_t ui_vframes_fill;
    uint32_t ui_out_of_vframes;
    uint32_t ui_max_vframe;

#if defined(__x86_64__) && defined(__OBJ_PE__)
    uint8_t *ui_xdata_start;
    uint8_t *ui_xdata_end;
    uint8_t *ui_pdata_start;
    uint8_t *ui_pdata_end;
#endif

#if defined(__x86_64__)
    uint64_t ui_pvi_vmread;
    uint64_t ui_pvi_vmwrite;
#endif

#if defined(UXEN_HOST_OSX) || defined(__APPLE__)
    uint32_t ui_smap_enabled;
#endif

    uint32_t host_os_is_xmm_clean;
};

struct domain;
struct vcpu;

struct vm_vcpu_info_shared {
    uint32_t vci_vcpuid;
    uint32_t vci_runnable;
    uint32_t vci_run_mode;
    uint32_t vci_host_halted;
    uint32_t vci_has_timer_interrupt;
    void * volatile vci_wait_event;
    uint32_t vci_map_page_range_requested;
};

enum {
    VCI_RUN_MODE_IDLE,
    VCI_RUN_MODE_SETUP,
    VCI_RUN_MODE_PROCESS_IOREQ,
    VCI_RUN_MODE_PREEMPT,
    VCI_RUN_MODE_HALT,
    VCI_RUN_MODE_IDLE_WORK,
    VCI_RUN_MODE_YIELD,
    VCI_RUN_MODE_PAGEMAP_CHECK,
    VCI_RUN_MODE_FREEPAGE_CHECK,
    VCI_RUN_MODE_MAP_PAGE_REQUEST,
    VCI_RUN_MODE_VFRAMES_CHECK,
    VCI_RUN_MODE_SHUTDOWN,
};

struct vm_info_shared {
    xen_domain_handle_t vmi_uuid;
    domid_t vmi_domid;
    uint32_t vmi_runnable;
    uint32_t vmi_nrpages;
    uint64_t vmi_msrpm;
    uint32_t vmi_msrpm_size;
    uint64_t vmi_xsave;
    uint32_t vmi_xsave_size;
    void *vmi_domain;
    void *vmi_dmreq;
    void *vmi_dmreq_vcpu_page_va;
    void *vmi_dmreq_hec;
    void * volatile vmi_wait_event;
};

#define UXEN_PAGE_SHIFT 12
#define UXEN_PAGE_SIZE (1UL << UXEN_PAGE_SHIFT)
#define UXEN_PAGE_MASK (~(UXEN_PAGE_SIZE - 1))

#endif
