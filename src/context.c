@ {
    #ifdef APP_USE_SYSTEM_UCONTEXT
    #include <ucontext.h>
    #include <sys/mman.h>
    typedef ucontext_t exec_context;
    #else
    #include <fcontext.h>
    typedef fcontext_t exec_context;
    #endif
    
    #ifndef APP_USE_SYSTEM_UCONTEXT
    
    typedef struct {
        fcontext_t ctx;
        ct_task* task;
    } ct_transfer;
    
    #define ct_arg ct_transfer
    #define ct_unpack(arg) (arg.task)
    #define ctx_init(arg) jump_fcontext(arg.ctx, NULL)
    #define ctx_swap(save, to) ontop_fcontext(*(to), save, fcontext_ontop_wrapper)
    #define ctx_make(storage, ptr, sz, f, arg)                  \
        ({                                                      \
            __auto_type _ctx = storage;                         \
            *_ctx = make_fcontext(ptr, sz, (pfn_fcontext) f);   \
            *_ctx = jump_fcontext(*_ctx, arg).ctx;              \
        })
    #define ctx_stack_alloc(sz) create_fcontext_stack(sz)
    #define ctx_stack_free(ptr, sz) destroy_fcontext_stack(ptr, sz)
    #define ctx_stack_align(sz) aligned_fcontext_stack(sz)
    
    #else /* ucontext */
    
    #define ct_arg ct_task*
    #define ct_unpack(arg) (arg)

    #define ctx_init(arg) do {} while(0)
    
    #define ctx_swap(save, to) swapcontext(save, to)
    #define ctx_make(ctx, ptr, sz, f, arg)              \
        ({                                              \
            __auto_type _ctx = ctx;                     \
            getcontext(_ctx);                           \
            _ctx->uc_stack.ss_sp = ptr;                 \
            _ctx->uc_stack.ss_size = sz;                \
            _ctx->uc_stack.ss_flags = 0;                \
            makecontext(_ctx, (void (*)()) f, 1, arg);  \
        })
    #define ctx_stack_alloc(sz)                                 \
        mmap(NULL, sz, PROT_READ | PROT_WRITE,                  \
             MAP_ANONYMOUS | MAP_PRIVATE | MAP_STACK, -1, 0)
    #define ctx_stack_free(ptr, sz) munmap(ptr, sz)
    #define ctx_stack_align(sz) (sz)
    #endif
}
