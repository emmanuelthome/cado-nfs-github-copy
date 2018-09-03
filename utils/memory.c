
#include "cado.h"
#include <stdio.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <ctype.h>
#include <pthread.h>
#ifdef HAVE_SSE2
#include <emmintrin.h>
#endif

#ifdef HAVE_SYS_MMAN_H
#include <sys/mman.h>
#endif

/* For MinGW Build */
#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#endif

#include "macros.h"
#include "portability.h"
#include "misc.h"
#include "memory.h"
#include "dllist.h"

#ifndef LARGE_PAGE_SIZE
#define LARGE_PAGE_SIZE (2UL*1024*1024)
#endif

void
*malloc_check (const size_t x)
{
    void *p;
    p = malloc (x);
    if (p == NULL)
      {
        fprintf (stderr, "Error, malloc of %zu bytes failed\n", x);
        fflush (stderr);
        abort ();
      }
    return p;
}

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

/* A list of mmap()-ed and malloc()-ed memory regions, so we can call the
   correct function to free them again */
static dllist mmapped_regions, malloced_regions;
static pthread_mutex_t mmapped_regions_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t malloced_regions_lock = PTHREAD_MUTEX_INITIALIZER;

static pthread_once_t lists_init_control = PTHREAD_ONCE_INIT;

void lists_init()
{
    pthread_mutex_lock(&mmapped_regions_lock);
    dll_init(mmapped_regions);
    pthread_mutex_unlock(&mmapped_regions_lock);
    pthread_mutex_lock(&malloced_regions_lock);
    dll_init(malloced_regions);
    pthread_mutex_unlock(&malloced_regions_lock);
}

void * malloc_hugepages(size_t) ATTR_ASSUME_ALIGNED(32);
void * malloc_hugepages(const size_t size)
{
    pthread_once(&lists_init_control, &lists_init);

#if defined(HAVE_MMAP) && defined(MAP_HUGETLB)
  {
    size_t nr_pages = iceildiv(size, LARGE_PAGE_SIZE);
    size_t rounded_up_size = nr_pages * LARGE_PAGE_SIZE; 
    /* Start by trying mmap() */
    void *m = mmap (NULL, rounded_up_size, PROT_READ|PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE | MAP_HUGETLB, -1, 0);
    if (m == MAP_FAILED) {
      // Commented out because it's spammy
      // perror("mmap failed");
    } else {
      pthread_mutex_lock(&mmapped_regions_lock);
      dll_append(mmapped_regions, m);
      pthread_mutex_unlock(&mmapped_regions_lock);
      return m;
    }
  }
#endif

#ifdef MADV_HUGEPAGE
  {
    size_t nr_pages = iceildiv(size, LARGE_PAGE_SIZE);
    size_t rounded_up_size = nr_pages * LARGE_PAGE_SIZE; 
    /* If mmap() didn't work, try aligned malloc() with madvise() */
#if defined(__linux) && defined(HAVE_POSIX_MEMALIGN)
    /* add a fence a la electric fence from the good old days */
    void *m = malloc_aligned(rounded_up_size + 0x1000, LARGE_PAGE_SIZE);
    mprotect(m + rounded_up_size, 0x1000, PROT_NONE);
#else
    void *m = malloc_aligned(rounded_up_size, LARGE_PAGE_SIZE);
#endif
    pthread_mutex_lock(&malloced_regions_lock);
    dll_append(malloced_regions, m);
    pthread_mutex_unlock(&malloced_regions_lock);
    int r;
    static int printed_error = 0;
    do {
      r = madvise(m, rounded_up_size, MADV_HUGEPAGE);
    } while (r == EAGAIN);
    if (r != 0 && !printed_error) {
      perror("madvise failed");
      printed_error = 1;
    }
    return m;
  }
#endif

  /* If all else fails, return regular page-aligned memory */
  return malloc_pagealigned(size);
}

void
free_hugepages(void *m, const size_t size MAYBE_UNUSED)
{
  if (m == NULL)
    return;

#if defined(HAVE_MMAP) && defined(MAP_HUGETLB)
  {
    size_t nr_pages = iceildiv(size, LARGE_PAGE_SIZE);
    size_t rounded_up_size = nr_pages * LARGE_PAGE_SIZE; 
    pthread_mutex_lock(&mmapped_regions_lock);
    dllist_ptr node = dll_find (mmapped_regions, (void *) m);
    if (node != NULL) {
      dll_delete(node);
      pthread_mutex_unlock(&mmapped_regions_lock);
      munmap((void *) m, rounded_up_size);
      return;
    }
    pthread_mutex_unlock(&mmapped_regions_lock);
  }
#endif
  
#ifdef MADV_HUGEPAGE
  {
    size_t nr_pages = iceildiv(size, LARGE_PAGE_SIZE);
    size_t rounded_up_size = nr_pages * LARGE_PAGE_SIZE; 
    pthread_mutex_lock(&malloced_regions_lock);
    dllist_ptr node = dll_find (malloced_regions, (void *) m);
    if (node != NULL) {
      dll_delete(node);
      pthread_mutex_unlock(&malloced_regions_lock);
#if defined(__linux) && defined(HAVE_POSIX_MEMALIGN)
      /* we must remove the memory protection we had ! */
      mprotect(m + rounded_up_size, 0x1000, PROT_READ|PROT_WRITE|PROT_EXEC);
#endif
      free_aligned(m);
      return;
    }
    pthread_mutex_unlock(&malloced_regions_lock);
  }
#endif
  free_pagealigned(m);
}

void
*physical_malloc (const size_t x, const int affect)
{
  void *p;
  p = malloc_hugepages(x);
  if (affect) {
    size_t i, m;
#ifdef HAVE_SSE2
    const __m128i a = (__m128i) {0, 0};
#endif    
    i = ((size_t) p + 15) & (~15ULL);
    m = ((size_t) p + x - 1) & (~15ULL);
    while (i < m) {
#ifdef HAVE_SSE2
      _mm_stream_si128((__m128i *)i, a);
#else
      *(unsigned char *) i = 0;
#endif
      i += pagesize ();
    }
  }
  return p;
}

void
physical_free(void *m, const size_t size)
{
  free_hugepages(m, size);
}

/* Not everybody has posix_memalign. In order to provide a viable
 * alternative, we need an ``aligned free'' matching the ``aligned
 * malloc''. We rely on posix_memalign if it is available, or else fall
 * back on ugly pointer arithmetic so as to guarantee alignment. Note
 * that not providing the requested alignment can have some troublesome
 * consequences. At best, a performance hit, at worst a segv (sse-2
 * movdqa on a pentium4 causes a GPE if improperly aligned).
 */

void *malloc_aligned(size_t size, size_t alignment)
{
#ifdef HAVE_POSIX_MEMALIGN
    void *res = NULL;
    int rc = posix_memalign(&res, alignment, size);
    // ASSERT_ALWAYS(rc == 0);
    DIE_ERRNO_DIAG(rc != 0, "malloc_aligned", "");
    return res;
#else
    char * res;
    res = malloc(size + sizeof(size_t) + alignment);
    res += sizeof(size_t);
    size_t displ = alignment - ((uintptr_t) res) % alignment;
    res += displ;
    memcpy(res - sizeof(size_t), &displ, sizeof(size_t));
    ASSERT_ALWAYS((((uintptr_t) res) % alignment) == 0);
    return (void*) res;
#endif
}

/* Reallocate aligned memory.
   p must have been allocated via malloc_aligned() or realloc_aligned().
   old_size must be equal to the size parameter of malloc_aligned(), or
   to the new_size parameter of realloc_aligned(), of the function that
   allocated p. */

void *
realloc_aligned(void * p, const size_t old_size, const size_t new_size,
                const size_t alignment)
{
#ifdef HAVE_POSIX_MEMALIGN
  /*  Alas, there is no posix_realloc_aligned(). Try to realloc(); if it
      happens to result in the desired alignment, there is nothing left
      to do. If it does not result in the desired alignment, then we
      actually do two data copies: one as part of realloc(), and another
      below. Let's hope this happens kinda rarely. */
  p = realloc(p, new_size);
  if (((uintptr_t) p) % alignment == 0) {
    return p;
  }
#else
  /* Without posix_memalign(), we always alloc/copy/free */
#endif
  /* We did not get the desired alignment, or we don't have posix_memalign().
     Allocate new memory with the desired alignment and copy the data */
  void * const alloc_p = malloc_aligned(new_size, alignment);
  memcpy(alloc_p, p, MIN(old_size, new_size));
  /* If we have posix_memalign(), then p was allocated by realloc() and can be
     freed with free(), which is what free_aligned() does. If we don't have
     posix_memalign(), then p was allocated by malloc_aligned() or
     realloc_aligned(), so using free_aligned() is correct again. */
  free_aligned(p);
  return alloc_p;
}


void free_aligned(void * p)
{
#ifdef HAVE_POSIX_MEMALIGN
    free((void *) p);
#else
    if (p == NULL)
      return;
    const char * res = (const char *) p;
    size_t displ;
    memcpy(&displ, res - sizeof(size_t), sizeof(size_t));
    res -= displ;
    res -= sizeof(size_t);
    free((void *)res);
#endif
}

void *malloc_pagealigned(size_t sz)
{
    void *p = malloc_aligned (sz, pagesize ());
    ASSERT_ALWAYS(p != NULL);
    return p;
}

void free_pagealigned(void * p)
{
    free_aligned(p);
}

/* Functions for allocating contiguous physical memory, if huge pages are available.
   If not, they just return malloc_pagealigned().
   
   We keep track of allocated memory with a linked list. No attempt is made to fill
   freed holes with later requests, new requests always get memory after the 
   furthest-back allocated chunk. We assume that all of the contiguous memory needed
   by a program is allocated at the start, and all freed at the end, so that this
   restriction has no impact. */

struct chunk_s {
  void *ptr;
  size_t size;
};

static void *hugepages = NULL;
static size_t hugepage_size_allocated = 0;
static dllist chunks;
static pthread_mutex_t chunks_lock = PTHREAD_MUTEX_INITIALIZER;

// static pthread_once_t chunks_init_control = PTHREAD_ONCE_INIT;

// #define VERBOSE_CONTIGUOUS_MALLOC 1

static void chunks_init()
{
    // pthread_mutex_lock(&chunks_lock);
    dll_init(chunks);

    ASSERT_ALWAYS(hugepages == NULL);

    /* Allocate one huge page. Can allocate more by assigning a larger value
       to hugepage_size_allocated. */
    hugepage_size_allocated = LARGE_PAGE_SIZE;
    hugepages = malloc_hugepages(hugepage_size_allocated);
    ASSERT_ALWAYS(hugepages != NULL);
    // pthread_mutex_unlock(&chunks_lock);

#ifdef VERBOSE_CONTIGUOUS_MALLOC
    printf ("# Allocated %zu bytes of huge page memory at = %p\n",
            hugepage_size_allocated, hugepages);
#endif
}

void *contiguous_malloc(const size_t size)
{
  if (size == 0) {
    return NULL;
  }

  // pthread_once(&chunks_init_control, &chunks_init);

  /* Get offset and size of last entry in linked list */
  void *free_ptr;
  size_t free_size;
  pthread_mutex_lock(&chunks_lock);
  if (hugepages == NULL) chunks_init();
  if (!dll_is_empty(chunks)) {
    struct chunk_s *chunk = dll_get_nth(chunks, dll_length(chunks) - 1)->data;
    free_ptr = (char *)(chunk->ptr) + chunk->size;
  } else {
    free_ptr = hugepages;
  }
  pthread_mutex_unlock(&chunks_lock);
  free_size = hugepage_size_allocated - ((char *)free_ptr - (char *)hugepages);

  /* Round up to a multiple of 128, which should be a (small) multiple of the
     cache line size. Bigger alignment should not be necessary, if the memory
     is indeed backed by a huge page. */
  const size_t round_up_size = iceildiv(size, 128) * 128;
  /* See if there is enough memory in the huge page left to serve the
     current request */
  if (round_up_size > free_size) {
    /* If not, return memory allocated by malloc_pagealigned */
    void *ptr = malloc_pagealigned(size);
#ifdef VERBOSE_CONTIGUOUS_MALLOC
    printf ("# Not enough huge page memory available, returning malloc_pagealigned() = %p\n", ptr);
#endif
    return ptr;
  }

  struct chunk_s *chunk = (struct chunk_s *) malloc(sizeof(struct chunk_s));
  ASSERT_ALWAYS(chunk != NULL);
  chunk->ptr = free_ptr;
  chunk->size = round_up_size;
  pthread_mutex_lock(&chunks_lock);
  dll_append(chunks, chunk);
  pthread_mutex_unlock(&chunks_lock);

#ifdef VERBOSE_CONTIGUOUS_MALLOC
  printf ("# Returning %zu bytes of huge-page memory at %p\n", chunk->size, chunk->ptr);
#endif
  return free_ptr;
}

void contiguous_free(void *ptr)
{
  dllist_ptr node;
  struct chunk_s *chunk;
  
  if (ptr == NULL)
    return;
  
  pthread_mutex_lock(&chunks_lock);
  for (node = chunks->next; node != NULL; node = node->next) {
    chunk = node->data;
    if (chunk->ptr == ptr) {
      break;
    }
  }
  pthread_mutex_unlock(&chunks_lock);

  if (node != NULL) {
#ifdef VERBOSE_CONTIGUOUS_MALLOC
    printf ("# Freeing %zu bytes of huge-page memory at %p\n", 
            chunk->size, chunk->ptr);
#endif
    /* we do not free chunk->ptr yet: that is done when the pool is
     * completely drained.
     */
    free(chunk);
    pthread_mutex_lock(&chunks_lock);
    dll_delete(node);
    if (dll_is_empty(chunks)) {
#ifdef VERBOSE_CONTIGUOUS_MALLOC
      printf ("# Last chunk freed, freeing huge page\n");
#endif
      free_hugepages(hugepages, hugepage_size_allocated);
      hugepages = NULL;
      hugepage_size_allocated = 0;
    }
    pthread_mutex_unlock(&chunks_lock);
  } else {
#ifdef VERBOSE_CONTIGUOUS_MALLOC
    printf ("# huge-page memory at %p not found, calling free_pagealigned()\n", ptr);
#endif
    free_pagealigned(ptr);
  }
}
