/**
   Simple auto-resizing ringbuffer type.
*/
@ {
    #include <stdlib.h>
    #include <stdint.h>

    #include <threading.h>
    #include <time.h>
    
    typedef struct {
        uint8_t* data;
        size_t buf_sz, at, bottom;
    } ringbuf;

    /* Packed values for performing single CAS operations on both atomically */
    typedef union {
        uint64_t raw;
        struct __attribute__((__packed__)) {
            uint32_t index;
            uint32_t writing;
        } s;
    } atomic_ringbuf_istore;

    /*
      Atomic ringbuffers must contain pointer members (for atomic operations). We also
      declare atomic ringbuffers with included storage for its members to improve cache
      locality in hot code that makes usage of atomic ringbuffers.
      
      The enqueue/dequeue implementation is completely wait-free and outperforms other
      queue implementations due to assuming some usage limitations:
      
      - Members must be atomically sized for CAS (usually 8 or 16 bytes), meaning complex
      message passing requires pointers to data with lifetime guarantees.
      
      - Only one consumer is permitted (MPSC)
      
      - Allocation space must be 2^n, and should contain enough space to allow elements
      marked as read to remain in the buffer, with at most `num_threads` garbage values
      remaining. This is due to pending writes delaying element removal beyond a marked
      index, however pending writes _do not block other writes or reads_, meaning the
      queue remains responsive even with preempted writer threads.
      
      Due to the strong guarantees provided (thread preemption mid-write only delays removal,
      otherwise progress in every other writer and consumer continues), this implementation
      is probably as optimal as it gets for the sake of scheduling.
      
      Field explanation:
      
      data: bounded set of elements of atomic size
      bottom: read index for consumer
      top_free: marker value which indicates to the consumer which elements can be cleared
      pending_remove: number of elements waiting to be cleared for consumer
      reserved: count of positions reserved in the buffer; used only for detecting when full
      waiting: count of producer threads waiting on a full buffer; used only for weak signaling
      u.s.index: "soft" write index for producers
      u.s.writing: concurrent write counter, used to update `top_free` when == 0
    */
    
    #define QUEUE_CACHE_LINE_SIZE 64
    #define WFMPSC_P_TYPE(sz, type, tname)                              \
        typedef type tname##_type;                                      \
        typedef struct tname {                                          \
            union {                                                     \
                type value;                                             \
                struct {                                                \
                    uint8_t section[QUEUE_CACHE_LINE_SIZE];             \
                } _block;                                               \
            } data[sz];                                                 \
            uint32_t bottom         __attribute__((aligned(QUEUE_CACHE_LINE_SIZE))); \
            uint32_t top_free       __attribute__((aligned(QUEUE_CACHE_LINE_SIZE))); \
            uint32_t pending_remove __attribute__((aligned(QUEUE_CACHE_LINE_SIZE))); \
            uint32_t reserved       __attribute__((aligned(QUEUE_CACHE_LINE_SIZE))); \
            uint32_t waiting        __attribute__((aligned(QUEUE_CACHE_LINE_SIZE))); \
            atomic_ringbuf_istore u __attribute__((aligned(QUEUE_CACHE_LINE_SIZE))); \
            async_mutex mutex       __attribute__((aligned(QUEUE_CACHE_LINE_SIZE))); \
            async_cond cond;                                            \
        } tname
    
    /*
    typedef uint8_t wfmpsc_state;
    #define WFMPSC_TYPE(sz, type, tname)                                \
        typedef type tname##_type;                                      \
        union tname##_slot;                                             \
        typedef union tname##_slot {                                    \
            struct {                                                    \
                wfmpsc_state state;                                     \
                type data;                                              \
                union wfmpsc_slot* next;                                \
            } value;                                                    \
            struct {                                                    \
                uint8_t section[QUEUE_CACHE_LINE_SIZE];                 \
            } _block;                                                   \
        } tname##_slot;                                                 \
        typedef struct tname {                                          \
            tname##_slot data[sz];                                      \
            uint32_t bottom         __attribute__((aligned(QUEUE_CACHE_LINE_SIZE))); \
            uint32_t top_free       __attribute__((aligned(QUEUE_CACHE_LINE_SIZE))); \
            uint32_t pending_remove __attribute__((aligned(QUEUE_CACHE_LINE_SIZE))); \
            uint32_t reserved       __attribute__((aligned(QUEUE_CACHE_LINE_SIZE))); \
            uint32_t waiting        __attribute__((aligned(QUEUE_CACHE_LINE_SIZE))); \
            atomic_ringbuf_istore u __attribute__((aligned(QUEUE_CACHE_LINE_SIZE))); \
            async_mutex mutex       __attribute__((aligned(QUEUE_CACHE_LINE_SIZE))); \
            async_cond cond;                                            \
        } tname
    */
    
    #define WFMPSC_P_DEF_FUNCS_STATIC(tname, placeholder)               \
        static const tname##_type tname##_placeholder = placeholder;    \
        static tname##_type tname##_pop(tname* buf) {                   \
            return wfmpsc_p_pop(buf, placeholder);                      \
        }                                                               \
        static void tname##_push(tname* buf, tname##_type value) {      \
            wfmpsc_p_push(buf, value, placeholder);                     \
        }                                                               \
        static inline void tname##_init(tname* buf) {                   \
            wfmpsc_p_init(buf, tname);                                  \
        }                                                               \
        static inline void tname##_destroy(tname* buf) {                \
            wfmpsc_p_destroy(buf);                                      \
        }

    #define WFMPSC_P_DEF_FUNCS_STATIC_RAW(tname, placeholder)           \
        static const tname##_type tname##_placeholder = placeholder;    \
        static tname##_type tname##_pop(tname* buf) {                   \
            return wfmpsc_p_pop_r(buf, placeholder);                    \
        }                                                               \
        static void tname##_push(tname* buf, tname##_type value) {      \
            wfmpsc_p_push_r(buf, value, placeholder);                   \
        }                                                               \
        static inline void tname##_init(tname* buf) {                   \
            wfmpsc_p_init(buf, tname);                                  \
        }                                                               \
        static inline void tname##_destroy(tname* buf) {                \
            wfmpsc_p_destroy(buf);                                      \
        }
        
    #define WFMPSC_P_DEF_FUNCS(tname)                           \
        tname##_type tname##_pop(tname* buf) {                  \
            return wfmpsc_p_pop(buf, tname##_placeholder);      \
        }                                                       \
        void tname##_push(tname* buf, tname##_type value) {     \
            wfmpsc_p_push(buf, value, tname##_placeholder);     \
        }

    #define WFMPSC_P_DEF_FUNCS_RAW(tname)                       \
        tname##_type tname##_pop(tname* buf) {                  \
            return wfmpsc_p_pop_r(buf, tname##_placeholder);    \
        }                                                       \
        void tname##_push(tname* buf, tname##_type value) {     \
            wfmpsc_p_push_r(buf, value, tname##_placeholder);   \
        }
        
    #define WFMPSC_P_DECL_FUNCS(tname, placeholder)                     \
        static const tname##_type tname##_placeholder = placeholder;    \
        tname##_type tname##_pop(tname* buf);                           \
        void tname##_push(tname* buf, tname##_type value);              \
        static inline void tname##_init(tname* buf) {                   \
            wfmpsc_p_init(buf, tname);                                  \
        }                                                               \
        static inline void tname##_destroy(tname* buf) {                \
            wfmpsc_p_destroy(buf);                                      \
        }

    #define WFMPSC_P_INIT(tname)                                        \
        ((tname) {                                                      \
            .u              = { .s = { .index = 0, .writing = 0 } },    \
            .bottom         = 0,                                        \
            .top_free       = 0,                                        \
            .pending_remove = 0,                                        \
            .reserved       = 0,                                        \
            .waiting        = 0                                         \
        })

    #define wfmpsc_p_init(storage, tname)           \
        ({                                          \
            *storage = WFMPSC_P_INIT(tname);        \
            async_cond_init(&storage->cond);        \
            async_mutex_init(&storage->mutex);      \
        })

    #define wfmpsc_p_destroy(storage)               \
        ({                                          \
            async_cond_destroy(&storage->cond);     \
            async_mutex_destroy(&storage->mutex);   \
        })
    
    static inline void ringbuf_init(ringbuf* r, size_t buf_sz, size_t elem_sz) {
        r->data = malloc(buf_sz * elem_sz);
        r->buf_sz = buf_sz;
        r->at = 0;
        r->bottom = 0;
    }
    
    
    static inline void ringbuf_insert(ringbuf* r, void* elem, size_t elem_sz) {
        memcpy(r->data + (r->at * elem_sz), elem, elem_sz);
        ++r->at; /* write index */
        if (r->at >= r->buf_sz) {
            r->at = 0;
        }
        /* if the read and write index are the same, we ran out of space */
        if (r->at == r->bottom) {
            r->data = realloc(r->data, (r->buf_sz * 2) * elem_sz);  /* double buffer allocation     */
            memcpy(r->data + ((r->at + r->buf_sz) * elem_sz),       /* dest: [at..buf_sz) -> buf_sz */
                   r->data + (r->at * elem_sz), r->buf_sz - r->at); /* src:  [at..buf_sz)           */
            r->bottom += r->buf_sz; /* shift read pointer to new part       */
            r->buf_sz *= 2;         /* update actual buffer allocation size */
        }
    }

    #define ringbuf_empty(r) ({ __auto_type _r = r; _r->at == _r->bottom; }) 
    
    #define ringbuf_destroy(r) free((r)->data)
    
    #define ringbuf_get(r, elem_sz) ({ __auto_type _r = r; (_r->data + (_r->bottom * elem_sz)); })
    
    #define ringbuf_consume(r)                              \
        ({                                                  \
            __auto_type _r = r;                             \
            if (_r->bottom != _r->at)                       \
                _r->bottom = (_r->bottom + 1) % _r->buf_sz; \
        })
    
    /*
      We attempt to use a CAS instruction instead of `__atomic_load`
      due to some platforms (notably x86_64) only supporting 16 byte atomics
      with CAS instructions.
      
      GCC doesn't provide these workarounds since they break immutability, which
      we don't care about.
      
      On 32-bit systems this support isn't nessecary since pointers are 4 bytes,
      so a 8 byte CAS can handle two pointers just fine.
      
      todo: assert other architectures do not need this workaround (ppc64)
      todo: aarch64 likely needs this as well: <https://gcc.gnu.org/bugzilla/show_bug.cgi?id=70814>,
            however aarch64 has the extra annoyance of failed atomic reads/writes.
      (GCC will actually print a nice type compatibility error if load/store support for __int128
      doesn't exist)
    */
    #ifdef __x86_64__
    #define _arch_store(addr, val, memorder)                            \
        ({                                                              \
            __auto_type _addr = addr;                                   \
            __atomic_compare_exchange_n(_addr, _addr, (val), false, memorder, memorder); \
        })
    
    #define _arch_load(addr, memorder)                                  \
        ({                                                              \
            __auto_type _addr = addr;                                   \
            typeof(*_addr) _store;                                      \
            __atomic_compare_exchange(&_store, &_store,                 \
                                      _addr, false, memorder, memorder); \
            _store;                                                     \
        })
    #else
    #define _arch_store(addr, val, memorder) __atomic_store_n(addr, val, memorder)
    #define _arch_load(addr, memorder) __atomic_load_n(addr, memorder)
    #endif
    
    /*
      atomic ringbuffer pop implementation
      
      - only allows for single consumers, but this is generally not an issue for queues
      - does not read write index, instead checks exchange result for placeholder
      - lazily removes elements according to `top_free`; preventing interference with
      pending reads (due to writes relying on placeholder values in CAS loop), while
      still allowing writes from other threads to be read.
    
      Due to how lazy element removal works, some space in the ringbuffer before the bottom
      index may remain allocated with non-placeholder (garbage) values, to be removed at a
      later invocation.
      
      Note the code `if (_r->bottom == _top_free % _buf_sz) { ... }`, which handles an edge case
      when the ringbuffer is completely read and the read index is equal to the marker index. In
      this scenario, the marker can refer to two states: no writes pending, or pending an increment
      of the buffer size. Because extra write attempts are detected, the end result is the same
      (incrementing the marker by the buffer size leaves it in the same position).
      
      We solve this edge case by "declogging" the buffer; removing the element at the index, putting
      the buffer back into a valid state. This fix works because the ABA read/write ordering problem
      doesn't actually apply when no other valid write positions exist (plus, all positions are
      reserved, so pending writes are actually blocked).
      
      This macro should not be used directly, use `RINGBUF_ATOMIC_FUNCS` to define functions instead.
    */
    #define wfmpsc_p_pop(r, placeholder)                                \
        ({                                                              \
            __auto_type _r = r;                                         \
            typeof((*(_r->data)).value) _pp = placeholder;              \
            size_t _buf_sz = (sizeof(_r->data) / sizeof(typeof(*(_r->data)))); \
            __auto_type _ret = _arch_load(&_r->data[_r->bottom].value, __ATOMIC_ACQUIRE); \
            uint32_t _top_free = __atomic_load_n(&_r->top_free, __ATOMIC_ACQUIRE); \
            if (memcmp(&_ret, &_pp, sizeof(_ret)) && _r->pending_remove < _buf_sz) { \
                ++(_r->pending_remove);                                 \
                _r->bottom = (_r->bottom + 1) % _buf_sz;                \
            } else {                                                    \
                if (_r->pending_remove >= _buf_sz) {                    \
                    _ret = _pp;                                         \
                    if (_r->bottom == _top_free % _buf_sz) {            \
                        _arch_store(&_r->data[_r->bottom].value, _pp, __ATOMIC_SEQ_CST); \
                        __atomic_fetch_sub(&_r->reserved, 1, __ATOMIC_SEQ_CST); \
                        async_cond_signal(&_r->cond);                   \
                        --(_r->pending_remove);                         \
                    }                                                   \
                }                                                       \
                uint32_t _off;                                          \
                bool _passed_free_marker = false;                       \
                for (uint32_t t = _r->pending_remove; t > 0; --t) {     \
                    _off = (_r->bottom - t) % _buf_sz;                  \
                    if (_top_free % _buf_sz == _off)                    \
                        _passed_free_marker = true;                     \
                    if (!_passed_free_marker) {                         \
                        _arch_store(&_r->data[_off].value, _pp, __ATOMIC_SEQ_CST); \
                        __atomic_fetch_sub(&_r->reserved, 1, __ATOMIC_SEQ_CST); \
                        if (__atomic_load_n(&_r->waiting, __ATOMIC_SEQ_CST)) \
                            async_cond_signal(&_r->cond);               \
                        --(_r->pending_remove);                         \
                    }                                                   \
                }                                                       \
            }                                                           \
            _ret;                                                       \
        })
    
    #define wfmpsc_p_pop_r(r, placeholder)                              \
        ({                                                              \
            __auto_type _r = r;                                         \
            typeof((*(_r->data)).value.raw) _pp = (placeholder).raw;    \
            size_t _buf_sz = (sizeof(_r->data) / sizeof(typeof(*(_r->data)))); \
            __auto_type _ret = _arch_load(&_r->data[_r->bottom].value.raw, __ATOMIC_ACQUIRE); \
            uint32_t _top_free = __atomic_load_n(&_r->top_free, __ATOMIC_ACQUIRE); \
            if (memcmp(&_ret, &_pp, sizeof(_ret)) && _r->pending_remove < _buf_sz) { \
                ++(_r->pending_remove);                                 \
                _r->bottom = (_r->bottom + 1) % _buf_sz;                \
            } else {                                                    \
                if (_r->pending_remove >= _buf_sz) {                    \
                    _ret = _pp;                                         \
                    if (_r->bottom == _top_free % _buf_sz) {            \
                        _arch_store(&_r->data[_r->bottom].value.raw, _pp, __ATOMIC_SEQ_CST); \
                        __atomic_fetch_sub(&_r->reserved, 1, __ATOMIC_SEQ_CST); \
                        async_cond_signal(&_r->cond);                   \
                        --(_r->pending_remove);                         \
                    }                                                   \
                }                                                       \
                uint32_t _off;                                          \
                bool _passed_free_marker = false;                       \
                for (uint32_t t = _r->pending_remove; t > 0; --t) {     \
                    _off = (_r->bottom - t) % _buf_sz;                  \
                    if (_top_free % _buf_sz == _off)                    \
                        _passed_free_marker = true;                     \
                    if (!_passed_free_marker) {                         \
                        _arch_store(&_r->data[_off].value.raw, _pp, __ATOMIC_SEQ_CST); \
                        __atomic_fetch_sub(&_r->reserved, 1, __ATOMIC_SEQ_CST); \
                        if (__atomic_load_n(&_r->waiting, __ATOMIC_SEQ_CST)) \
                            async_cond_signal(&_r->cond);               \
                        --(_r->pending_remove);                         \
                    }                                                   \
                }                                                       \
            }                                                           \
            (typeof((*(_r->data)).value)) { .raw = _ret };                                          \
        })

    /*
      atomic ringbuffer push implementation:
  
      increment write counter
      load write index
      while (!cmpexch(&data[local index], &NULL, elem))
      do {
      load write index, write counter (packed)
      } while (!cmpexch(&packed, &local packed, packed { write index + 1, write counter - 1 }))
      if (write counter + 1 == 0) {
      top_free = local write index
      }
      
      entirely avoids ABA problem:
      
      - if multiple threads load the same write index, only one cmpexch operation succeeds
      - competing threads simply move to the next position to attempt another atomic write
      - data is fully written by the time the write index is incremented
      - a `top_free` value is updated when an invocation completes without pending writes,
      communicating to the queue consumer that it may remove values below that point
      without interfering with cmpexch write functionality.
    
      write count & write index is updated atomically in a single cmpexch loop, which
      allows the code to assume at _least_ the `top_free` value can be updated to the set
      write index when the write count reaches zero. This is implemented by packing the
      two 32-bit values together and performing a 64-bit cmpexch.
      
      space is reserved (in no particular order) purely to maintain a count of slots to
      handle when the buffer is full. In this scenario, we perform a <=5ms wait that can
      be (unreliably) signaled from the consumer thread. Because the consumer does not lock
      access to `waiting` with a mutex, there is a potential for missed signals, so a max
      sleep duration is required to handle this rare edge case.
      
      Because the missed signal edge case can only apply when producers fill the buffer faster
      than it can be cleared and requires producers to wait anyway, overall performance is
      unaffected.
      
      tradeoffs:
      
      - very fast implementation (wait free), but requires the underlying type to be sized for atomics
      - reserves placeholder value for atomic functionality (both push/pop use this)
      - does not resize
      - lazy element removal is performed in the consumer based on pending writes, meaning free
      space is hard to reason about in code

      This macro should not be used directly, use `RINGBUF_ATOMIC_FUNCS` to define functions instead.
    */
    #define wfmpsc_p_push(r, elem, placeholder)                        \
        ({                                                              \
            __auto_type _r = r;                                         \
            typeof((*(_r->data)).value) _elem = elem;                   \
            size_t _buf_sz = (sizeof(_r->data) / sizeof(typeof(*(_r->data)))); \
        _attempt:                                                       \
            uint32_t _res = __atomic_add_fetch(&_r->reserved, 1, __ATOMIC_SEQ_CST); \
            if (_res > _buf_sz) {                                       \
                __atomic_sub_fetch(&_r->reserved, 1, __ATOMIC_SEQ_CST); \
                __atomic_add_fetch(&_r->waiting, 1, __ATOMIC_SEQ_CST);  \
                async_mutex_lock(&_r->mutex);                           \
                struct timespec _ts;                                     \
                timespec_get(&_ts, TIME_UTC);                            \
                _ts.tv_nsec += 5 * 1000000;                              \
                async_cond_timedwait(&_r->cond, &_r->mutex, &_ts);       \
                async_mutex_unlock(&_r->mutex);                         \
                __atomic_sub_fetch(&_r->waiting, 1, __ATOMIC_SEQ_CST);  \
                goto _attempt;                                          \
            } else {                                                    \
                __atomic_add_fetch(&_r->u.s.writing, 1, __ATOMIC_SEQ_CST); \
                size_t _aat_store = __atomic_load_n(&_r->u.s.index, __ATOMIC_SEQ_CST); \
                size_t _aat = _aat_store % _buf_sz;                     \
                typeof((*(_r->data)).value) _null = (typeof((*(_r->data)).value)) (placeholder); \
                while (!__atomic_compare_exchange_n(&_r->data[_aat].value, &_null, \
                                                    _elem, false,       \
                                                    __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) { \
                    _null = (typeof((*(_r->data)).value)) (placeholder); \
                    _aat = (_aat + 1) % _buf_sz;                        \
                }                                                       \
                atomic_ringbuf_istore _expected, _uv;                   \
                do {                                                    \
                    _expected.raw = __atomic_load_n(&_r->u.raw, __ATOMIC_SEQ_CST); \
                    _uv.s.index   = _expected.s.index + 1;              \
                    _uv.s.writing = _expected.s.writing - 1;            \
                } while (!__atomic_compare_exchange_n(&_r->u.raw, &_expected.raw, _uv.raw, false, \
                                                      __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)); \
                if (_uv.s.writing == 0) {                               \
                    __atomic_store_n(&_r->top_free, _uv.s.index, __ATOMIC_RELEASE); \
                }                                                       \
            }                                                           \
        })
    
    #define wfmpsc_p_push_r(r, elem, placeholder)                       \
        ({                                                              \
            __auto_type _r = r;                                         \
            typeof((*(_r->data)).value.raw) _elem = elem.raw;           \
            size_t _buf_sz = (sizeof(_r->data) / sizeof(typeof(*(_r->data)))); \
        _attempt:                                                       \
            uint32_t _res = __atomic_add_fetch(&_r->reserved, 1, __ATOMIC_SEQ_CST); \
            if (_res > _buf_sz) {                                       \
                __atomic_sub_fetch(&_r->reserved, 1, __ATOMIC_SEQ_CST); \
                __atomic_add_fetch(&_r->waiting, 1, __ATOMIC_SEQ_CST);  \
                async_mutex_lock(&_r->mutex);                           \
                struct timespec _ts;                                     \
                timespec_get(&_ts, TIME_UTC);                            \
                _ts.tv_nsec += 5 * 1000000;                              \
                async_cond_timedwait(&_r->cond, &_r->mutex, &_ts);       \
                async_mutex_unlock(&_r->mutex);                         \
                __atomic_sub_fetch(&_r->waiting, 1, __ATOMIC_SEQ_CST);  \
                goto _attempt;                                          \
            } else {                                                    \
                __atomic_add_fetch(&_r->u.s.writing, 1, __ATOMIC_SEQ_CST); \
                size_t _aat_store = __atomic_load_n(&_r->u.s.index, __ATOMIC_SEQ_CST); \
                size_t _aat = _aat_store % _buf_sz;                     \
                typeof((*(_r->data)).value.raw) _null = (typeof((*(_r->data)).value.raw)) (placeholder).raw; \
                while (!__atomic_compare_exchange_n(&_r->data[_aat].value.raw, &_null, \
                                                    _elem, false,       \
                                                    __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) { \
                    _null = (typeof((*(_r->data)).value.raw)) (placeholder).raw; \
                    _aat = (_aat + 1) % _buf_sz;                        \
                }                                                       \
                atomic_ringbuf_istore _expected, _uv;                   \
                do {                                                    \
                    _expected.raw = __atomic_load_n(&_r->u.raw, __ATOMIC_SEQ_CST); \
                    _uv.s.index   = _expected.s.index + 1;              \
                    _uv.s.writing = _expected.s.writing - 1;            \
                } while (!__atomic_compare_exchange_n(&_r->u.raw, &_expected.raw, _uv.raw, false, \
                                                      __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)); \
                if (_uv.s.writing == 0) {                               \
                    __atomic_store_n(&_r->top_free, _uv.s.index, __ATOMIC_RELEASE); \
                }                                                       \
            }                                                           \
        })

    /*
      Helper function for iterating generic atomic ringbuffers.
    */
    #define wfmpsc_p_iter(r, elem, tname)                               \
        __auto_type _r = r;                                             \
        for (typeof((*(_r->data)).value) elem;                          \
        (elem = (typeof((*(_r->data)).value)) tname##_pop(_r)) != tname##_placeholder;)

    #define wfmpsc_p_iter_cmp(r, elem, tname, cmp)                      \
        __auto_type _r = r;                                             \
        for (typeof((*(_r->data)).value) elem;                          \
        cmp((elem = (typeof((*(_r->data)).value)) tname##_pop(_r)), tname##_placeholder);)
    
    #define wfmpsc_p_iter_r(r, elem, tname)                             \
        __auto_type _r = r;                                             \
        for (typeof((*(_r->data)).value.raw) elem;                          \
        (elem = (typeof((*(_r->data)).value.raw)) tname##_pop(_r)) != tname##_placeholder;)

    #define wfmpsc_p_iter_cmp_r(r, elem, tname, cmp)                    \
        __auto_type _r = r;                                             \
        for (typeof((*(_r->data)).value.raw) elem;                          \
        cmp((elem = (typeof((*(_r->data)).value.raw)) tname##_pop(_r)), tname##_placeholder);)

    #define ringbuf_iter(r, elem, type)                                 \
        __auto_type _r = r;                                             \
        for (type* elem = (type*) ringbuf_get(_r, sizeof(type)); _r->bottom != _r->at; \
             ({                                                         \
                 ringbuf_consume(_r);                                   \
                 elem = (type*) ringbuf_get(_r, sizeof(type));          \
             }))
}
