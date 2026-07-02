/* Kernel heap — malloc/free over page_alloc + the M3 VMM (milestone-5).
 * A contiguous virtual arena in the on-demand mapping region that grows
 * by mapping fresh physical pages; this is the backend the EigenScript
 * freestanding heap (malloc/calloc/realloc/free) binds to. */
#ifndef EIGENOS_KHEAP_H
#define EIGENOS_KHEAP_H

#include <stddef.h>

void  kheap_init(void);
void *kmalloc(size_t n);
void *kcalloc(size_t nmemb, size_t size);
void *krealloc(void *p, size_t n);
void  kfree(void *p);

size_t kheap_used_bytes(void);      /* live (allocated) payload bytes */
size_t kheap_mapped_bytes(void);    /* arena size currently backed by pages */

#endif /* EIGENOS_KHEAP_H */
