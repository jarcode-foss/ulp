/*
  The MIT License (MIT)

  Copyright (c) 2016 Sepehr Taghdisian

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in all
  copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
  SOFTWARE.
  
  END LICENSE
  
  The original source for this code should be available at https://github.com/septag/deboost.context
  Some formatting modifications have been made.
*/

#ifndef APP_USE_SYSTEM_UCONTEXT

#include <assert.h>
#include <math.h>
#include <string.h>

#include <fcontext.h>

@ {
    #include <stdint.h>
    #include <stddef.h>
    
    typedef void* fcontext_t;

    typedef struct {
        fcontext_t ctx;
        void* data;
    } fcontext_transfer_t;

    /**
     * Callback definition for context (coroutine)
     */
    typedef void (*pfn_fcontext)(fcontext_transfer_t);

    /**
     * Switches to another context
     * @param to Target context to switch to
     * @param vp Custom user pointer to pass to new context
     */
    fcontext_transfer_t jump_fcontext(fcontext_t const to, void * vp);

    /**
     * Make a new context
     * @param sp Pointer to allocated stack memory
     * @param size Stack memory size
     * @param corofn Callback function for context (coroutine)
     */
    fcontext_t make_fcontext(void * sp, size_t size, pfn_fcontext corofn);

    fcontext_transfer_t ontop_fcontext(fcontext_t const to, void * vp, fcontext_transfer_t(*fn)(fcontext_transfer_t));
}

// Detect posix
#if !defined(_WIN32) && (defined(__unix__) || defined(__unix) || (defined(__APPLE__) && defined(__MACH__)))
/* UNIX-style OS. ------------------------------------------- */
#   include <unistd.h>
#   define _HAVE_POSIX 1
#endif

#ifdef _WIN32
#   define WIN32_LEAN_AND_LEAN
#   include <windows.h>
/* x86_64
 * test x86_64 before i386 because icc might
 * define __i686__ for x86_64 too */
#if defined(__x86_64__) || defined(__x86_64) \
    || defined(__amd64__) || defined(__amd64) \
    || defined(_M_X64) || defined(_M_AMD64)
/* Windows seams not to provide a constant or function
 * telling the minimal stacksize */
#   define MINSIGSTKSZ  8192
#else
#   define MINSIGSTKSZ  4096
#endif

static size_t system_page_size() {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (size_t) si.dwPageSize;
}

static size_t system_min_stack_size() {
    return MINSIGSTKSZ;
}

static size_t system_max_stack_size() {
    return  1 * 1024 * 1024 * 1024; /* 1GB */
}

static size_t system_default_stack_size() {
    return 131072;  // 128kb
}

#elif defined(_HAVE_POSIX)
#include <signal.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>

#if !defined (SIGSTKSZ)
# define SIGSTKSZ 131072 // 128kb recommended
# define UDEF_SIGSTKSZ
#endif

#if !defined (MINSIGSTKSZ)
# define MINSIGSTKSZ 32768 // 32kb minimum
# define UDEF_MINSIGSTKSZ
#endif

static size_t system_page_size() {
    /* conform to POSIX.1-2001 */
    return (size_t)sysconf(_SC_PAGESIZE);
}

static size_t system_min_stack_size() {
    return MINSIGSTKSZ;
}

static size_t system_max_stack_size() {
    struct rlimit limit;
    getrlimit(RLIMIT_STACK, &limit);

    return (size_t)limit.rlim_max;
}

static size_t system_default_stack_size() {
    return SIGSTKSZ;
}
#endif

@ size_t aligned_fcontext_stack(size_t size) {
    size_t pages;
    if (size == 0)
        size = system_default_stack_size();
    size_t minsz = system_min_stack_size();
    size_t maxsz = system_max_stack_size();
    if (size < minsz)
        size = minsz;
    if (size > maxsz)
        size = maxsz;

    pages = (size_t) floorf((float) size / (float) system_page_size());
    assert(pages >= 2);     /* at least two pages must fit into stack (one page is guard-page) */

    size_t ret = pages * system_page_size();
    assert(ret != 0 && size != 0);
    assert(ret <= size);
    return ret;
}

/* Stack allocation and protection*/
@ void* create_fcontext_stack(size_t size) {
    size_t size_;
    void* vp;
    
    size_ = aligned_fcontext_stack(size);
    
    #ifdef _WIN32
    vp = VirtualAlloc(0, size_, MEM_COMMIT, PAGE_READWRITE);
    if (!vp)
        return NULL;

    DWORD old_options;
    VirtualProtect(vp, system_page_size(), PAGE_READWRITE | PAGE_GUARD, &old_options);
    #elif defined(_HAVE_POSIX)
    #if defined(MAP_ANON)
    #if defined(MAP_UNINITIALIZED)
    vp = mmap(0, size_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_STACK | MAP_ANON | MAP_UNINITIALIZED, -1, 0);
    #else
    vp = mmap(0, size_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_STACK | MAP_ANON, -1, 0);
    #endif
    #else
    #if defined(MAP_UNINITIALIZED)
    vp = mmap(0, size_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_STACK | MAP_ANONYMOUS | MAP_UNINITIALIZED, -1, 0);
    #else
    vp = mmap(0, size_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_STACK | MAP_ANONYMOUS, -1, 0);
    #endif
    #endif
    if (vp == MAP_FAILED)
        return NULL;
    mprotect(vp, system_page_size(), PROT_NONE);
    #else
    vp = malloc(size_);
    if (!vp)
        return NULL;
    #endif
    
    return (char*) vp + size_;
}

@ void destroy_fcontext_stack(void* sptr, size_t ssize) {
    
    // ssize = aligned_fcontext_stack(ssize);
    void* vp;

    assert(ssize >= system_min_stack_size());
    assert(ssize <= system_max_stack_size());

    vp = (char*)sptr - ssize;

#ifdef _WIN32
    VirtualFree(vp, 0, MEM_RELEASE);
#elif defined(_HAVE_POSIX)
    munmap(vp, ssize);
#else
    free(vp);
#endif
}

#ifdef UDEF_SIGSTKSZ
#   undef SIGSTKSZ
#endif

#ifdef UDEF_MINSIGSTKSZ
#   undef MINSIGSTKSZ
#endif

#endif /* APP_USE_SYSTEM_UCONTEXT */
