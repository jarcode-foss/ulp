/*
  High performance O(1) coroutine/greenthreading interface. Uses pure userspace context switching when
  it is available for the host target (see src/asm/... files), otherwise defaults to `ucontext.h`.
  
  This implementation avoids as much indirection as possible, uses vectors with pinned elements for
  tasks, performs scheduling in-place such that only one context switch is required for most yield/resume
  interactions, provides functionality for completing multiple futures, and timed/repeating tasks.
  
  Tasks that are both timed and repeating use an algorithm for O(1) scheduling that requires
  "nice" intervals with a low LCM, since execution is pre-computed upon creation.
  
  Lua integration is also part of the scheduler itself, so it can be used just like Lua coroutines,
  except this scheduler backs tasks with an actual stack.
*/
#include <sched.h>
#include <ct.h>

@ {
    typedef int future;
    typedef struct ct_task ct_task;
    typedef struct ct_scheduler ct_scheduler;
    
    #include <luabase.h>
    #include <stdint.h>
    #include <time.h>
    #include <threading.h>
    #include <ring.h>
    #include <context.h>

    /* Many optimizations can be performed with a compile-time task limit */
    #define CT_TASKLIMIT 4096
    // #define CT_TASKLIMIT_REPEAT (CT_TASKLIMIT * 4)
    #define CT_FUTURELIMIT 128
    
    #define CT_REPEAT (uint8_t) 1 << 0
    #define CT_TIMED  (uint8_t) 1 << 1
    #define CT_LUA    (uint8_t) 1 << 2
    /* used internally */
    #define CT_REMOVE (uint8_t) 1 << 3
    /* used internally, DEPRECATED */
    #define CT_RESUME (uint8_t) 1 << 4

    #define FUTURE_MAXCOMPLETE 16
    #define FUTURE_INIT 0
    #define FUTURE_WAITING 0
    #define FUTURE_OK 1
    #define FUTURE_ERR 2

    /* This structure is used in an atomic ringbuffer; on x64 it is 16 bytes so some platforms
       may not offer the 16 byte CAS instructions required to deal with it. Targets with
       4-byte sized pointers can fit this structure into 8 bytes, however, making atomics easier. */
    #ifdef __LP64__
    typedef __int128 ct_future_ret_raw;
    #else
    typedef uint64_t ct_future_ret_raw;
    #endif
    typedef union {
        ct_future_ret_raw raw;
        struct {
            future* ref;
            future value;
        } v;
    } ct_future_ret;
    
    WFMPSC_P_TYPE      (FUTURE_MAXCOMPLETE, ct_future_ret, ct_f_ringbuf);
    WFMPSC_P_DECL_FUNCS(ct_f_ringbuf, {});
    
    /* A single task, run at most once per tick (except for yield/resume scenarios) */
    typedef struct ct_task {
        bool          alive;
        uint8_t       flags;
        exec_context  context;
        int           yielded, write_count; /* atomic */
        uint64_t      interval;
        size_t        list_pos;
        void*         store;
        ct_scheduler* sched;
        lua_State*    lua_thread;
        void*         stack_alloc;
        future*       futures[FUTURE_MAXCOMPLETE];
        ct_f_ringbuf  futures_to_write __attribute__((aligned(16)));
    } ct_task;

    WFMPSC_P_TYPE      (CT_TASKLIMIT, ct_task*, ct_ringbuf);
    WFMPSC_P_DECL_FUNCS(ct_ringbuf, NULL);

    typedef struct {
        ct_task* task;
        struct timespec delay;
    } ct_repeat_calc;

    /* Cooperative greenthread scheduler; this large structure includes
       most buffers for execution for cache-friendly code. */
    typedef struct ct_scheduler {
        size_t          context_stack_sz;
        exec_context    main_context;
        ct_ringbuf      remove, resume;
        struct timespec repeat_started, timed_started;
        ct_repeat_calc  repeat[CT_TASKLIMIT];
        ct_repeat_calc  timed[CT_TASKLIMIT];
        size_t          repeat_sz, timed_sz, timed_bottom, each_sz,
                        each_first_dead_pos, repeat_first_dead_pos;
        ct_task*        each[CT_TASKLIMIT];
        size_t          stack_sz, stack_first_dead_pos;
        ct_task         stack[CT_TASKLIMIT];
        struct {
            int t, e, state;
            bool complete;
            struct timespec wait;
        } iter;
    } ct_scheduler;

    /*
      Async errors for file and networking operations are return to the future itself,
      encoding the system error/GetLastError value with an offset defined by a macro.
      
      For performance, none of the error values go through a translation table for compat,
      so some macros are needed to handle how the underlying platform signals certain errors.
     */
    #define ASYNC_ERR_OFFSET 12
    #ifdef _WIN32
    #define FUTURE_NOT_FOUND (ERROR_FILE_NOT_FOUND + ASYNC_ERR_OFFSET)
    #define FUTURE_BAD_DRIVE (ERROR_INVALID_DRIVE + ASYNC_ERR_OFFSET)
    /* note: the following two macros are the same value to allow for blindly checking both macros */
    #define FUTURE_NOT_PERMITTED (ERROR_ACCESS_DENIED + ASYNC_ERR_OFFSET)
    #define FUTURE_ACCESS_DENIED (ERROR_ACCESS_DENIED + ASYNC_ERR_OFFSET)
    #define FUTURE_BAD_FILE (ERROR_INVALID_HANDLE + ASYNC_ERR_OFFSET)
    #else
    /* note: the following two macros are the same value to allow for blindly checking both macros */
    #define FUTURE_NOT_FOUND (ENOENT + ASYNC_ERR_OFFSET)
    #define FUTURE_BAD_DRIVE (ENOENT + ASYNC_ERR_OFFSET)
    #define FUTURE_NOT_PERMITTED (EPERM + ASYNC_ERR_OFFSET)
    #define FUTURE_ACCESS_DENIED (EACCES + ASYNC_ERR_OFFSET)
    #define FUTURE_BAD_FILE (EBADF + ASYNC_ERR_OFFSET)
    #endif
    
    #define FUTURE_CHECKFOUND(x)                \
        ({                                      \
            __auto_type _x = x;                 \
            _x != FUTURE_NOT_FOUND &&           \
                _x != FUTURE_BAD_DRIVE;         \
        })
    
    #define FUTURE_CHECKPERMS(x)                \
        ({                                      \
            __auto_type _x = x;                 \
            _x != FUTURE_NOT_PERMITTED &&       \
                _x != FUTURE_ACCESS_DENIED;     \
        })
    /*
      Macros for handling error returns from future, only to be used for debug builds.
      This is useful for asserting the validity of IO code.
    */
    #ifdef APP_DEBUG
    #define errfail_abort(errstr)                                       \
        do {                                                            \
            fprintf(stderr, "[" __FILE__ ":%d] FATAL: %s\n", __LINE__, errstr); \
            exit(EXIT_FAILURE);                                         \
        } while (0)
    #define debug_errfail(future)                   \
        do {                                        \
            if (future != FUTURE_OK)                \
                errfail_abort(future_str(future));  \
        } while (0)
    #else
    #define debug_errfail(future) do {} while (0)
    #endif
}

WFMPSC_P_DEF_FUNCS(ct_ringbuf);
WFMPSC_P_DEF_FUNCS_RAW(ct_f_ringbuf);

#define TASK_STACK_SIZE (4096 * 1024)
#define ITER_STATE_REPEAT 0
#define ITER_STATE_EACH 1
#define ITER_STATE_TIMED 3
#define ITER_STATE_RESUME 4
#define ITER_STATE_REMOVE 5
#define ITER_STATE_PROCESS 6

static const char* errmsg_generic = "Generic error";
static const char* errmsg_unknown = "Unknown error";
static const char* errmsg_waiting = "Waiting";
static const char* errmsg_ok      = "Ok";
/**
   Return a string to represent the state of the given future. Uses system functions
   for generating error messages if required, otherwise it returns an immutable internal
   string.
   
   future_str_free() should always be called on the result
 */
@ const char* future_str(future future) {
    switch (future) {
        case FUTURE_OK:      return errmsg_ok;
        case FUTURE_ERR:     return errmsg_generic;
        case FUTURE_WAITING: return errmsg_waiting;
        default: {
            future -= ASYNC_ERR_OFFSET;
            #ifdef _WIN32
            LPSTR buf;
            if(FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                              FORMAT_MESSAGE_IGNORE_INSERTS, NULL, (DWORD) future,
                              MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR) &buf, 0, NULL)) {
                return (const char*) buf;
            }
            #else
            #ifdef _GNU_SOURCE
            char buf[256];
            const char* s = strerror_r(future, buf, sizeof(buf));
            if (s)
                return strdup(s);
            #else
            char buf[256];
            if (!strerror_r(future, buf, sizeof(buf)))
                return strdup(buf);
            #endif
            #endif
            return errmsg_unknown;
        }
    }
}

@ const void future_str_free(const char* fmt) {
    if (fmt == errmsg_generic || fmt == errmsg_unknown
        || fmt == errmsg_waiting || fmt == errmsg_ok)
        return;
    #ifdef _WIN32
    LocalFree((LPTSTR) fmt);
    #else
    free((char*) fmt);
    #endif
}

@ {
    #ifndef APP_USE_SYSTEM_UCONTEXT
    fcontext_transfer_t fcontext_ontop_wrapper(fcontext_transfer_t arg);
    #endif
}
#ifndef APP_USE_SYSTEM_UCONTEXT
fcontext_transfer_t fcontext_ontop_wrapper(fcontext_transfer_t arg) {
    *((fcontext_t*) arg.data) = arg.ctx;
    return (fcontext_transfer_t) {};
}
#endif

@(extern) __thread lua_State*    ct_lua_state    = NULL;
@(extern) __thread ct_scheduler* ct_current      = NULL;
@(extern) __thread ct_task*      ct_current_task = NULL;

@ ct_scheduler* ct_init(ct_scheduler* stack) {
    memset(stack, 0, sizeof(ct_scheduler));
    stack->context_stack_sz = ctx_stack_align(TASK_STACK_SIZE);
    ct_ringbuf_init(&stack->resume);
    ct_ringbuf_init(&stack->remove);
    // async_queue_create(&stack->queue);
    // memset(stack->stack, 0, sizeof(ct_task) * 16);
    #ifdef APP_USE_SYSTEM_UCONTEXT
    getcontext(&stack->main_context);
    #endif
    return stack;
    
}

@ void ct_destroy(ct_scheduler* stack) {
    ct_ringbuf_destroy(&stack->resume);
    ct_ringbuf_destroy(&stack->remove);
    // async_queue_destroy(&stack->queue);
}

static inline void alloc_tlist(ct_scheduler* sched, ct_task* task) {
    ct_repeat_calc* list = sched->timed;
    size_t o = sched->timed_bottom;
    size_t* list_sz = &sched->timed_sz;
    if (*list_sz == CT_TASKLIMIT) {
        fprintf(stderr, "FATAL: reached `CT_TASKLIMIT` (alloc_tlist) (%d)\n", CT_TASKLIMIT);
        exit(EXIT_FAILURE);
    }
    struct timespec target;
    timespec_get(&target, TIME_UTC);
    target = ts_add(target, ts_fms(task->interval));
    /* binary search */
    size_t a, b, j, sz = *list_sz;
    for (a = 0, b = sz; a < b;) {
        j = a + (b - a) / 2;
        if (ts_compare(target, <=, list[(j + o) % CT_TASKLIMIT].delay))
            b = j;
        else
            a = j + 1;
    }
    if (a < sz && ts_compare(list[(a + o) % CT_TASKLIMIT].delay, >=, target)) {
        for (int t = (sz - a); t >= 0; --t)
            list[(t + a + 1 + o) % CT_TASKLIMIT] = list[(t + a + o) % CT_TASKLIMIT];
    }
    ++(*list_sz);
    list[(a + o) % CT_TASKLIMIT] = (ct_repeat_calc) { .task = task, .delay = target };
}

static inline ct_task* pop_tlist(ct_scheduler* sched) {
    size_t* o = &sched->timed_bottom;
    ct_task* task = NULL;
    struct timespec target;
    timespec_get(&target, TIME_UTC);
    if (ts_compare(sched->timed[*o].delay, <, target)) {
        task = sched->timed[*o].task;
        sched->timed[*o] = (ct_repeat_calc) { .task = NULL };
        *o = (*o + 1) % CT_TASKLIMIT;
        --(sched->timed_sz);
    }
    return task;
}

/*
  (OUTDATED DESC)
  
  Repeat tasks are pre-calculated into a set of scheduled invocations over a
  period equal to the LCM of all task intervals. This has a couple tradeoffs:
  
  * O(1) execution
  * O(n) insertion and deletion
  * large invocation buffer for large LCM values, "nice" intervals required
  
  The purpose of this implementation is to ensure extremely high execution
  performance since repeat tasks are encouraged to be long-lived, leveraging
  the nature of cooperative threading to vastly outperform preemptive threading.
*/
static void add_repeat(ct_scheduler* sched, ct_task* task) {
    ct_repeat_calc* list = sched->repeat;
    struct timespec target;
    timespec_get(&target, TIME_UTC);
    target = ts_add(target, ts_fms(task->interval));
    
    while (sched->repeat_first_dead_pos < sched->repeat_sz) {
        /* Search for gaps and use dead allocation if possible */
        size_t old_pos = (sched->repeat_first_dead_pos)++;
        if (list[old_pos].task == NULL) {
            list[old_pos] = (ct_repeat_calc) { .task = task, .delay = target };
            task->list_pos = old_pos;
            return;
        }
    }
    /* If the allocation ran out of space, panic */
    if (sched->repeat_sz > CT_TASKLIMIT) {
        fprintf(stderr, "FATAL: reached `CT_TASKLIMIT` (add_repeat) (%d)\n", CT_TASKLIMIT);
        exit(EXIT_FAILURE);
    }
    
    ++(sched->repeat_first_dead_pos);
    /* No gaps, shift size for new entry */
    size_t old_pos = sched->repeat_sz++;
    list[old_pos] = (ct_repeat_calc) { .task = task, .delay = target };
    task->list_pos = old_pos;
}
/*
  Removed tasks do not downsize the repeat period for performance reasons.
*/
static inline void remove_repeat(ct_scheduler* sched, ct_task* task) {
    ct_repeat_calc* list = sched->repeat;
    if (sched->repeat_first_dead_pos > task->list_pos)
        sched->repeat_first_dead_pos = task->list_pos;
    list[task->list_pos].task = NULL;
}

static inline void alloc_plist(ct_scheduler* sched, ct_task* task) {
    ct_task** list = sched->each;
    size_t* list_sz = &sched->each_sz;
    while (sched->each_first_dead_pos < *list_sz) {
        /* Search for gaps and use dead allocation if possible */
        size_t old_pos = (sched->each_first_dead_pos)++;
        if (list[old_pos] == NULL) {
            list[old_pos] = task;
            task->list_pos = old_pos;
            return;
        }
    }
    /* If the allocation ran out of space, panic */
    if (*list_sz >= CT_TASKLIMIT) {
        fprintf(stderr, "FATAL: reached `CT_TASKLIMIT` (alloc_plist) (%d)\n", CT_TASKLIMIT);
        exit(EXIT_FAILURE);
    }
    
    ++(sched->each_first_dead_pos);
    /* No gaps, shift size for new entry */
    size_t old_pos = (*list_sz)++;
    list[old_pos] = task;
    task->list_pos = old_pos;
}

static inline void free_plist(ct_scheduler* sched, ct_task* task) {
    ct_task** list = sched->each;
    if (sched->each_first_dead_pos > task->list_pos)
        sched->each_first_dead_pos = task->list_pos;
    list[task->list_pos] = NULL;
}

static ct_task* alloc_task(ct_scheduler* stack) {
    while (stack->stack_first_dead_pos < stack->stack_sz) {
        /* Search for gaps and use dead allocation if possible */
        ++stack->stack_first_dead_pos;
        if (!stack->stack[stack->stack_first_dead_pos - 1].alive) {
            return &stack->stack[stack->stack_first_dead_pos - 1];
        }
    }
    /* If the allocation ran out of space, panic */
    if (stack->stack_sz >= CT_TASKLIMIT) {
        fprintf(stderr, "FATAL: reached `CT_TASKLIMIT` (alloc_task) (%d)\n", CT_TASKLIMIT);
        exit(EXIT_FAILURE);
    }
    
    ++stack->stack_first_dead_pos;
    /* No gaps, shift size for new entry */
    return &stack->stack[stack->stack_sz++];
}


/**
   flag CT_REPEAT:
  
   When used with `CT_TIMED`, this flag attempts to repeat the task once every
   `interval` ms. Note: if it misses execution due to a previously yielded task, it waits
   until the next scheduled interval to attempt another invocation.
       
   For tasks using `CT_REPEAT` alone, invocation occurs for every iteration.
  
   flag CT_TIMED:
  
   When used alone, applies a timed delay before execution. See above for repeating.
  
   flag CT_LUA:
  
   Use for tasks that are expected to use Lua functionality. Slower task creation/removal,
   and has some associated memory in Lua state
  
   interval:
   - When used with CT_TIMED: delay or interval in milliseconds
   - When used without CT_TIMED, but with CT_REPEAT: ignored
   - When used without CT_TIMED or CT_REPEAT: 0 to execute in the current tick,
   1 to execute in the next tick
                                                    
   @param stack the scheduler to use
   @param f task function to use
   @param flags flags governing the behaviour of the task
   @param interval 
*/
@ ct_task* schedule(ct_scheduler* stack, void (*f)(ct_arg),
                    uint8_t flags, int interval) {
    ct_task* task = alloc_task(stack);
    task->sched = stack;
    task->flags = flags;
    task->interval = interval;
    ct_f_ringbuf_init(&task->futures_to_write);
    
    if (task->interval == 0) {
        ++task->interval;
    }
    
    if ((task->flags & CT_TIMED) != 0) {
        if ((task->flags & CT_REPEAT) != 0) {
            add_repeat(stack, task);
        } else {
            alloc_tlist(stack, task);
        }
    } else {
        if ((task->flags & CT_REPEAT) != 0) {
            alloc_plist(stack, task);
        } else {
            ct_ringbuf_push(&stack->resume, task);
        }
    }
    task->alive = true;
    task->stack_alloc = ctx_stack_alloc(TASK_STACK_SIZE);
    ctx_make(&task->context, task->stack_alloc, stack->context_stack_sz, f, task);
    
    if ((task->flags & CT_LUA) != 0) {
        lua_State* L = ct_current ? ct_lua_state : default_lua_state;
        task->lua_thread = lua_newthread(L);
        lua_pushlightuserdata(L, task->lua_thread);
        lua_pushvalue(L, -2);
        lua_rawset(L, LUA_REGISTRYINDEX);
        lua_pop(L, 1);
    }
    return task;
}

static void lua_schedule_wrapper(ct_arg task) {
    CT_BEGIN(task) {
        lua_State* L = ct_unpack(task)->lua_thread;
        lua_pushlightuserdata(L, ct_unpack(task));
        lua_rawget(L, LUA_REGISTRYINDEX);
        if (!lua_isfunction(L, -1)) {
            fprintf(stderr, "FATAL: no lua function at registry index for task\n");
            exit(EXIT_FAILURE);
        }
        lua_pushlightuserdata(L, ct_unpack(task));
        switch (lua_pcall(L, 1, 0, 0)) {
            case 0: break;
            case LUA_ERRRUN:
            case LUA_ERRMEM:
            default: {
                const char* ret = lua_tostring(L, -1);
                fprintf(stderr, "unexpected error running lua task: %s\n", ret);
                break;
            }
            case LUA_ERRERR: {
                fprintf(stderr, "error running non-existent error handler function (?)\n");
                break;
            }
        }
        lua_pushlightuserdata(L, ct_unpack(task));
        lua_pushnil(L);
        lua_rawset(L, LUA_REGISTRYINDEX);
    }
}

/* schedule(f, interval, flags...) */
static int luaS_schedule(lua_State* L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    int flags = CT_LUA;
    int interval = luaL_checkinteger(L, 2);
    for (int t = 3; t <= lua_gettop(L); ++t)
        flags |= luaL_checkinteger(L, t);
    ct_task* task = schedule(ct_current, lua_schedule_wrapper, flags, interval);
    lua_pushlightuserdata(L, task);
    lua_pushvalue(L, 1);
    lua_rawset(L, LUA_REGISTRYINDEX);
    lua_pushlightuserdata(L, task);
    return 1;
}
LUA_REGISTER_FUNCTION(luaS_schedule, "schedule");

@ {
    #define ct_update() ct_update_futures(ct_current_task, false)
}
@ int ct_update_futures(ct_task* task, bool check) {
    int r = 0;
    #define CMP(a, b) ({ a.raw != b.raw; })
    wfmpsc_p_iter_cmp(&task->futures_to_write, ret, ct_f_ringbuf, CMP) {
        *(ret.v.ref) = ret.v.value;
        if (check) {
            for (int t = 0; t < (sizeof(task->futures) / sizeof(future*)); ++t) {
                if (task->futures[t] == ret.v.ref) {
                    ++r;
                    task->futures[t] = NULL;
                    break;
                }
            }
        } else r = 1;
    }
    #undef CMP
    return r;
}

@ {
    /* Compile-time sugar for avoiding specifying the `sz` argument with `ct_complete` */
    #define PP_NARG(...)                        \
        PP_NARG_(__VA_ARGS__,PP_RSEQ_N())
    #define PP_NARG_(...)                       \
        PP_ARG_N(__VA_ARGS__)
    #define PP_ARG_N(                               \
        _1, _2, _3, _4, _5, _6, _7, _8, _9,_10,     \
        _11,_12,_13,_14,_15,_16,_17,_18,_19,_20,    \
        _21,_22,_23,_24,_25,_26,_27,_28,_29,_30,    \
        _31,_32,_33,_34,_35,_36,_37,_38,_39,_40,    \
        _41,_42,_43,_44,_45,_46,_47,_48,_49,_50,    \
        _51,_52,_53,_54,_55,_56,_57,_58,_59,_60,    \
        _61,_62,_63,  N, ...) N
    #define PP_RSEQ_N()                         \
        63,62,61,60,                            \
            59,58,57,56,55,54,53,52,51,50,      \
            49,48,47,46,45,44,43,42,41,40,      \
            39,38,37,36,35,34,33,32,31,30,      \
            29,28,27,26,25,24,23,22,21,20,      \
            19,18,17,16,15,14,13,12,11,10,      \
            9, 8, 7, 6, 5, 4, 3, 2, 1, 0

    /* Calls ct_complete_f with the size argument automatically specified */
    #define ct_complete(...) ct_complete_f(PP_NARG(__VA_ARGS__), __VA_ARGS__)
    
    /*
      Faster & inlined single-future versions of `ct_complete`, use `&_` for future storage:
      
      retval = ct_return(async_function(&_, "foobar"));
      ct_do(async_function(&_, "foobiz"));
    */
    #define ct_return(...)                                              \
        ({                                                              \
            future _ = FUTURE_INIT;                                     \
            __auto_type ret = __VA_ARGS__;                              \
            ct_task* _task = ct_current_task;                           \
            if (_ == FUTURE_WAITING) {                                  \
                _task->futures[0] = &_;                                 \
                __atomic_store_n(&_task->yielded, 1, __ATOMIC_SEQ_CST); \
                ct_iter(ct_current, _task, true);                       \
            }                                                           \
            ret;                                                        \
        })
    
    #define ct_do(...)                                                  \
        ({                                                              \
            future _ = FUTURE_INIT;                                     \
            __VA_ARGS__;                                                \
            ct_task* _task = ct_current_task;                           \
            if (_ == FUTURE_WAITING) {                                  \
                _task->futures[0] = &_;                                 \
                __atomic_store_n(&_task->yielded, 1, __ATOMIC_SEQ_CST); \
                ct_iter(ct_current, _task, true);                       \
            }                                                           \
        })
}
/**
   Yield for the provided futures to complete, expects list of `future*`. Ignores futures
   that are not in a `FUTURE_WAITING` state.
  
   The lifetime of the futures provided must survive up until their completion. In practise,
   this means simply keeping them on the stack and completing the futures within the same scope.
  
   Tasks can yield in between completions of futures, and can complete unrelated futures while
   previous futures are pending. Aborting tasks is not permitted with pending futures, however.
*/
@ void ct_complete_f(int sz, ...) {

    va_list args;
    va_start(args, sz);
    
    ct_task* task = ct_current_task;
    #ifdef APP_DEBUG
    if (task == NULL) {
        fprintf(stderr, "FATAL: completed outside of a task!\n");
        exit(EXIT_FAILURE);
    }
    #endif
    int asz = sz;

    /* Apply any deferred writes (doing so without parsing the future list is faster) */
    ct_update_futures(task, false);
    
    /* Note: any future flagged at this point will have writes deferred.
       If the futures are included in this complete call, this is an ABA scenario */
    for (int t = 0; t < sz; ++t) {
        future* arg = va_arg(args, future*);
        if (*arg == FUTURE_WAITING) 
            task->futures[t] = arg;
        else --asz;
    }
    /* Once the yield state is set, the task is considered to have no read/write access to the
       associated futures, since this signals to the worker threads that they can do so themselves. */
    __atomic_store_n(&task->yielded, asz, __ATOMIC_SEQ_CST);

    /* Spin if flag operations are still in effect (ABA mitigation); effectively "flushes" pending writes */
    while (__atomic_load_n(&task->write_count, __ATOMIC_SEQ_CST));
    
    /*
      Handle extra deferred writes from the initial ABA scenario.
       
       - Any direct writes before this operation, assuming there are pending deferred writes, will not
         schedule a resume, so we need to re-load the yield value.
    
       - Any direct writes after this operation, again assuming pending writes, will schedule a resume.
    */
    asz = __atomic_sub_fetch(&task->yielded, ct_update_futures(task, true), __ATOMIC_SEQ_CST);
    
    if (asz > 0)
        ct_iter(ct_current, task, true);
    
    va_end(args);
}

@ {
    #define ct_resume(task)                     \
        do {                                    \
            if (task->yielded == -1) {          \
                task->yielded = 0;              \
                ct_ringbuf_push(&ct_current->resume, task); \
            }                                   \
        } while (0)

    #define ct_resume_offload(sched, task)          \
        do {                                        \
            task->yielded = -3;                     \
            ct_ringbuf_push(&(sched)->resume, task); \
        } while (0)

    #define ct_return_offload(sched, task)          \
        do {                                        \
            task->yielded = 0;                      \
            ct_ringbuf_push(&(sched)->resume, task); \
        } while (0)
}

/** Flag a future that has been completed.
    
    Note: the future may not actually be updated when this function returns and can
    defer the write to a later point (see `ct_update()`) if the task is currently
    executing.
    
    Futures follow an ownership model where only one task/thread may have mutable access
    to its value. This is implied by task yield state.
*/
@ void ct_flag_future(ct_task* task, future* ref, future value) {
    __atomic_add_fetch(&task->write_count, 1, __ATOMIC_SEQ_CST);
    int yielded_store;
    if ((yielded_store = __atomic_load_n(&task->yielded, __ATOMIC_SEQ_CST)) != 0) {
        if (yielded_store > 0) {
            /* task is yielded on futures */
            for (int t = 0; t < (sizeof(task->futures) / sizeof(future*)); ++t) {
                if (task->futures[t] == ref) {
                    yielded_store = __atomic_sub_fetch(&task->yielded, 1, __ATOMIC_SEQ_CST);
                    task->futures[t] = NULL;
                    break;
                }
            }
        }
        *ref = value;
        if (yielded_store == 0)
            ct_ringbuf_push(&ct_current->resume, task);
    } else {
        /* task is executing (future unowned), defer write */
        ct_f_ringbuf_push(&task->futures_to_write,
                          (ct_future_ret) { .v = { .ref = ref, .value = value } });
    }
    __atomic_sub_fetch(&task->write_count, 1, __ATOMIC_SEQ_CST);
}

static int luaS_abort(lua_State* L) {
    ct_abort();
    return 0;
}
LUA_REGISTER_FUNCTION(luaS_abort, "abort");

static int luaS_yield(lua_State* L) {
    ct_yield();
    return 0;
}
LUA_REGISTER_FUNCTION(luaS_yield, "yield");

static int luaS_resume(lua_State* L) {
    luaL_checktype(L, 1, LUA_TLIGHTUSERDATA);
    ct_task* task = (ct_task*) lua_touserdata(L, 1);
    lua_settop(L, 1);
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (!lua_isfunction(L, -1)) {
        lua_pushstring(L, "tried to resume invalid task");
        lua_error(L);
    }
    ct_resume(task);
    return 0;
}
LUA_REGISTER_FUNCTION(luaS_resume, "resume");


/* (for yield) If the task is still yielded, that means either:
   - it's a single-run task sitting in the stack waiting for something
   - it's a repeat task that is waiting and encountered its next execution interval
   in the latter case, we just skip execution for the entire tick */

/* (for resume) flag this task to the current tick to indicate it has been started on this tick,
   otherwise resume without doing so and remove the resume request flag. */
#define prepare_execute(stack, task)                \
    ({                                              \
        bool ret = false;                           \
        if (task->yielded == 0) {                   \
            if ((task->flags & CT_LUA) != 0)        \
                ct_lua_state = task->lua_thread;    \
            ret = true;                             \
        }                                           \
        ret;                                        \
    })

#define assume_execute(stack, task)                 \
    ({                                              \
        if ((task->flags & CT_LUA) != 0)            \
            ct_lua_state = task->lua_thread;        \
    })

/* if a task tries to execute itself, this is a no-op */
#define execute(stack, task)                                            \
    do {                                                                \
        ct_task* self = ct_current_task;                                \
        if (self != task) {                                             \
            ct_current_task = task;                                     \
            ctx_swap(self ? &self->context : &stack->main_context, &task->context); \
            ct_current_task = self;                                     \
        }                                                               \
    } while(0)


/*
  Every scheduler function needs to setup use of the following macro accordingly:
  
  static void my_task(ct_arg task) {
      ct_begin(task) {
          ...
      }
  }
  
  The purpose of this pattern is to inline scheduler logic directly into tasks to allow
  for task switching with only one `ctx_swap` call.
*/
@ {
    #define CT_BEGIN(task)                                              \
        ctx_init(task);                                                 \
        ct_task* _task = ct_unpack(task);                               \
        bool _single = ((_task->flags & CT_REPEAT) == 0);               \
        ct_scheduler* _stack = ct_current;                              \
        goto _ct_after; _ct_start: ct_iter(_stack, _task, true); _ct_after: \
        for (;true; ({                                                  \
                    if (_single) {                                      \
                        _task->yielded = -2;                            \
                        ct_ringbuf_push(&_stack->remove, _task);        \
                        ct_iter(_stack, _task, false);                  \
                    } else {                                            \
                        goto _ct_start;                                 \
                    }                                                   \
                }))
}

/*
  Every time a task yields (whether manually or for a future), ends, or aborts, it calls
  into this function. As a result, the control flow of this function allows for this to
  act as a "resume" point for tasks that call into this with `once` set to true.

  The reason for this complex control flow that allows multiple tasks to progress the
  iterator themselves is simply performance. Using this strategy allows the use of a single
  context switch without relying on a specific scheduler context.
   
  This strategy can outperform "stackless" coroutine patterns due to greenthreaded context
  switching relying on longjmp-like behaviour instead of stack unwinding.
*/
/**
   Iterates through the underlying task stack for the scheduler. This function is reentrant.
*/
@ void ct_iter(ct_scheduler* sched, ct_task* current_task, bool once) {
    do {
        switch (sched->iter.state) {
            /* First, iterate through available repeat tasks */
            case ITER_STATE_REPEAT:
                if (sched->repeat_sz > 0) {
                    int t = sched->iter.t;
                    struct timespec current;
                    timespec_get(&current, TIME_UTC);
                    
                    if (t == 0) {
                        sched->repeat_started = current;
                    }
                    ct_repeat_calc* r = &sched->repeat[t];
                    ct_task* task = r->task;
                    
                    if (t == sched->repeat_sz - 1) {
                        sched->iter.state = ITER_STATE_EACH;
                        sched->iter.e = 0;
                        sched->iter.t = 0;
                    } else {
                        sched->iter.t++;
                    }
                    
                    if (ts_compare(sched->repeat_started, >, r->delay)) {
                        /* task ready to execute, attempt to enter */
                        if (prepare_execute(sched, task)) {
                            /* update target */
                            r->delay = ts_add(r->delay, ts_fms(task->interval));
                            execute(sched, task);
                            if (once || sched->iter.complete) return;
                        }
                        break;
                    }
                }
            case ITER_STATE_EACH: {
                size_t e = sched->iter.e++;
                if (e < sched->each_sz) {
                    if (sched->each[e] != NULL) {
                        ct_task* task = sched->each[e];
                        if (prepare_execute(sched, task)) {
                            execute(sched, task);
                            if (once || sched->iter.complete) return;
                        }
                    }
                    break;
                }
                sched->iter.state = ITER_STATE_TIMED;
            }
            case ITER_STATE_TIMED: {
                ct_task* task;
                if ((task = pop_tlist(sched)) != NULL) {
                    assume_execute(sched, task);
                    execute(sched, task);
                    if (once || sched->iter.complete) return;
                    break;
                } else sched->iter.state = ITER_STATE_RESUME;
            }
            case ITER_STATE_RESUME: {
                bool encountered_self = false;
                reset_resume:
                ct_task* task = ct_ringbuf_pop(&sched->resume);
                if (task) {
                    if (task->yielded == -3) {
                        /* special state which indicates thread wishes to be resumed in
                           a worker thread */
                        if (current_task != task) {
                            async_offload_f(task);
                            goto reset_resume;
                        } else {
                            /* If this task encounters itself in this state, that means
                               it is waiting to find another task to swap to (since a saved
                               context is needed to submit an offload command).

                               We solve this by pushing the task back onto the queue, continuing
                               iteration if there were any other tasks needing resuming. If we
                               pop this task *again*, then we can conclude the queue only contains
                               this task (edge case). */
                            ct_ringbuf_push(&sched->resume, task);
                            if (encountered_self) {
                                goto after_resume;
                            }
                            encountered_self = true;
                            goto reset_resume;
                        }
                    } else {
                        assume_execute(sched, task);
                        execute(sched, task);
                        if (once || sched->iter.complete) return;
                        break;
                    }
                } else {
                after_resume:
                    sched->iter.state = ITER_STATE_REMOVE;
                }
            }
            case ITER_STATE_REMOVE: {
                bool attempted_to_remove_self = false;
                wfmpsc_p_iter(&sched->remove, task, ct_ringbuf) {
                    if (ct_current_task != task) {
                        if ((task->flags & CT_REPEAT) != 0) {
                            if ((task->flags & CT_TIMED) != 0) {
                                remove_repeat(sched, task);
                            } else {
                                free_plist(sched, task);
                            }
                        } else {
                            /*
                              if ((task->flags & CT_TIMED) != 0) {
                              fprintf(stderr, "FATAL: tried to remove timed, non-repeat task\n");
                              exit(EXIT_FAILURE);
                              }
                            */
                        }
                        /* This used to be a function, but this was its only caller. */
                        // start remove_task
                        task->alive = false;
                        if ((task->flags & CT_LUA) != 0) {
                            lua_State* L = ct_current ? ct_lua_state : default_lua_state;
                            
                            lua_pushlightuserdata(L, task->lua_thread);
                            lua_pushnil(L);
                            lua_rawset(L, LUA_REGISTRYINDEX);
                            
                            lua_pushlightuserdata(L, task);
                            lua_pushnil(L);
                            lua_rawset(L, LUA_REGISTRYINDEX);
                        }
                        ctx_stack_free(task->stack_alloc, sched->context_stack_sz);
                        size_t idx = (size_t) (((uintptr_t) task) - ((uintptr_t) sched->stack))
                            / sizeof(ct_task);
                        if (sched->stack_first_dead_pos > idx)
                            sched->stack_first_dead_pos = idx;
                    } else {
                        attempted_to_remove_self = true;
                        // printf("todo: task tried to remove itself!\n");
                        // ct_ringbuf_push(&sched->remove, task);
                        // ctx_swap(&current_task->context, &sched->main_context);
                        // break;
                    }
                }
                if (attempted_to_remove_self)
                        ct_ringbuf_push(&sched->remove, ct_current_task);
                sched->iter.state = ITER_STATE_PROCESS;
            }
            case ITER_STATE_PROCESS: {
                
                struct timespec current;
                timespec_get(&current, TIME_UTC);
                // if (ts_compare(sched->iter.wait, >, current)) {
                // } else sched->iter.state = ITER_STATE_REPEAT;
                
                /* At this point, the iteration is over. We either return, or swap
                   to the main context if we're still in a task. */
                
                sched->iter.state = ITER_STATE_REPEAT;
                sched->iter.wait = ts_sub(sched->iter.wait, current);
                sched->iter.complete = true;
                if (current_task != NULL) {
                    ctx_swap(&current_task->context, &sched->main_context);
                }
                return;
            }
        }
    } while (true);
}

@ {
    static inline void ct_abort(void) {
        ct_task* task = ct_current_task;
        ct_scheduler* sched = ct_current;
        #ifdef APP_DEBUG
        if (task == NULL) {
            fprintf(stderr, "FATAL: aborted outside of a task!\n");
            exit(EXIT_FAILURE);
        }
        #endif
        task->yielded = -2;
        ct_ringbuf_push(&sched->remove, task);
        ct_iter(ct_current, task, false);
    }
}
/**
   Explicitly yields a task. Must be resumed with `ct_resume`.
*/
@ {
    static inline void ct_yield(void) {
        ct_task* task = ct_current_task;
        #ifdef APP_DEBUG
        if (task == NULL) {
            fprintf(stderr, "FATAL: yielded outside of a task!\n");
            exit(EXIT_FAILURE);
        }
        #endif
        __atomic_store_n(&task->yielded, -1, __ATOMIC_SEQ_CST);
        ct_iter(ct_current, task, true);
    }
}

@ void ct_enter(ct_scheduler* stack) {
    ct_lua_state = default_lua_state;
    ct_current = stack;
    stack->iter.complete = false;
    ct_iter(stack, NULL, false);
    ct_current = NULL;
    ct_lua_state = default_lua_state;
}
