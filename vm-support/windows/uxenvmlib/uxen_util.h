/*
 * Copyright (c) 2012, 2013, Christian Limpach <Christian.Limpach@gmail.com>
 * 
 * Permission to use, copy, modify, and/or distribute this software
 * for any purpose with or without fee is hereby granted, provided
 * that the above copyright notice and this permission notice appear
 * in all copies.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
 * OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
 * NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef _UXEN_UTIL_H_
#define _UXEN_UTIL_H_

#define UXEN_POOL_TAG 'uxen'

#define uxen_malloc(size)                                       \
    ExAllocatePoolWithTag(NonPagedPool, (size), UXEN_POOL_TAG)

#define uxen_free(addr)                                         \
    ExFreePoolWithTag(addr, UXEN_POOL_TAG)

void *
uxen_malloc_locked_pages(unsigned int nr_pages, unsigned int *mfn_list,
			 unsigned int max_mfn);
void *
uxen_user_map_page_range(unsigned int n, unsigned int *mfn, MDL **_mdl);

void
cpuid(uint32_t idx, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx);

void
wrmsr(uint32_t reg, uint64_t val);

#define PRId64 "Id"
#define PRIu64 "Iu"
#define PRIx64 "Ix"

int
uxen_DbgPrint(const char *fmt, ...);

struct shared_info *
uxen_get_shared_info(unsigned int *);

static __inline int uxen_is_whp_present_32(void) { return 0; }
int uxen_is_whp_present_64(void);

#ifdef __x86_64__
#define uxen_is_whp_present uxen_is_whp_present_64
#else
#define uxen_is_whp_present uxen_is_whp_present_32
#endif

#endif	/* _UXEN_UTIL_H_ */
