
#include <ct.h>
#include <threading.h>
#include <utf.h>

@ {
    #include <ring.h>
    #include <context.h>
    #ifdef _WIN32
    #include <winsock2.h>
    #include <windows.h>
    typedef HANDLE async_thread;
    typedef CONDITION_VARIABLE async_cond;
    typedef CRITICAL_SECTION async_mutex;
    #define async_entry_sig __attribute__((stdcall))
    #define async_entry_retval DWORD
    #define async_thread_return() do { return 0; } while(0)
    typedef async_entry_retval (async_entry_sig *async_thread_entry)(void*);
    #else
    #include <errno.h>
    #include <pthread.h>
    #include <unistd.h>
    #include <sys/types.h>
    #include <sys/stat.h>
    #include <fcntl.h>
    typedef pthread_t async_thread;
    typedef pthread_cond_t async_cond;
    typedef pthread_mutex_t async_mutex;
    #define async_entry_sig
    #define async_entry_retval void*
    #define async_thread_return() do { return NULL; } while(0)
    typedef async_entry_retval (*async_thread_entry)(void*);
    #endif
    
    #ifdef _WIN32
    typedef HANDLE async_fd;
    #else
    typedef int async_fd;
    #endif

    #define ASYNC_QUEUE_NUM_THREADS 4
    #define ASYNC_QUEUE_MAX 1024
    
    typedef struct {
        uint8_t id;
        future future;
        ct_task* task;
        union {
            struct {
                const char* path;
                async_fd* storage;
                int flags;
            } fs;
            struct {
                async_fd fd;
                void* ptr;
                size_t* ret;
                size_t sz;
                int extra, flags;
            } io;
            struct {
                exec_context ctx;
            } offload;
        } data;
    } async_cmd;
    
    WFMPSC_P_TYPE(ASYNC_QUEUE_MAX, async_cmd*, cmd_queue);
    
    struct async_queue;
    typedef struct async_queue_member {
        async_thread thread;
        struct async_queue* parent;
        bool iterating;
        bool running;
    } async_queue_member;
    
    typedef struct async_queue {
        struct async_queue_member members[ASYNC_QUEUE_NUM_THREADS];
        cmd_queue queue;
        size_t write;
        async_mutex mutex;
        async_cond cond;
    } async_queue;
}

static async_queue queue_storage;
WFMPSC_P_DEF_FUNCS_STATIC(cmd_queue, NULL);

@ {
    static inline void async_thread_create(async_thread* thread, async_thread_entry entry, void* arg) {
        #ifdef _WIN32
        *thread = CreateThread(NULL, 0, entry, arg, 0, NULL);
        #else
        pthread_create(thread, NULL, entry, arg);
        #endif
    }
    static inline void async_thread_name(async_thread* thread, const char* name) {
        #ifdef _WIN32
        //todo: impl
        #else
        //pthread_setname_np(*thread, name);
        #endif
    }
    #ifdef _WIN32
    #define async_mutex_init(mutex) InitializeCriticalSection(mutex)
    #define async_mutex_lock(mutex) EnterCriticalSection(mutex)
    #define async_mutex_unlock(mutex) LeaveCriticalSection(mutex)
    #define async_mutex_destroy(mutex) DeleteCriticalSection(mutex)
    #define async_cond_destroy(cond) do {} while (0)
    #define async_cond_init(cond) InitializeConditionVariable(cond)
    #define async_cond_signal(cond) WakeConditionVariable(cond)
    #define async_cond_broadcast(cond) WakeAllConditionVariable(cond)
    #define async_cond_wait(cond, mutex) SleepConditionVariableCS(cond, mutex, INFINITE)
    /* Windows has terrible resolution for condition variable waits; just round down to the
       nearest millisecond (and avoid an argument with zero since that actually performs a
       test instead of a wait, which could end up spinning threads needlessly with <1ms
       waits) */
    #define async_cond_timedwait(cond, mutex, tss)                      \
        ({                                                              \
            struct timespec* _tss = (tss);                              \
            struct timespec _s;                                         \
            timespec_get(&_s, TIME_UTC);                                \
            _s = ts_sub(*_tss, _s);                                     \
            SleepConditionVariableCS(cond, mutex, (_s.tv_sec * 1000) + (_s.tv_nsec / 1000000) \
                                     + ((_s.tv_sec == 0 && _s.tv_nsec < 1000000) ? 1 : 0)); \
        })
    #else
    #define async_mutex_init(mutex) pthread_mutex_init(mutex, NULL)
    #define async_mutex_lock(mutex) pthread_mutex_lock(mutex)
    #define async_mutex_unlock(mutex) pthread_mutex_unlock(mutex)
    #define async_mutex_destroy(mutex) pthread_mutex_destroy(mutex)
    #define async_cond_destroy(cond) pthread_cond_destroy(cond)
    #define async_cond_init(cond) pthread_cond_init(cond, NULL)
    #define async_cond_signal(cond) pthread_cond_signal(cond)
    #define async_cond_broadcast(cond) pthread_cond_broadcast(cond) 
    #define async_cond_wait(cond, mutex) pthread_cond_wait(cond, mutex)
    #define async_cond_timedwait(cond, mutex, ts) pthread_cond_timedwait(cond, mutex, ts)
    #endif
    static inline int async_thread_join(async_thread* thread) {
        #ifdef _WIN32
        if (WaitForSingleObject(*thread, INFINITE) == WAIT_OBJECT_0)
            return 0;
        else return -1;
        #else
        return pthread_join(*thread, NULL);
        #endif
    }

    #define ts_compare(a, OP, b)                                        \
        ({                                                              \
            struct timespec _a = a;                                     \
            struct timespec _b = b;                                     \
            (_a.tv_sec == _b.tv_sec) ? (_a.tv_nsec OP _b.tv_nsec) : (_a.tv_sec OP _b.tv_sec); \
        })

    #define ts_add(a, b)                                                \
        ({                                                              \
            struct timespec _a = a;                                     \
            struct timespec _b = b;                                     \
            (struct timespec)                                           \
            { .tv_sec = _a.tv_sec + _b.tv_sec + ((_a.tv_nsec + _b.tv_nsec) / 1000000000), \
                    .tv_nsec = (_a.tv_nsec + _b.tv_nsec) % 1000000000 }; \
        })

    #define ts_sub(a, b)                                                \
        ({                                                              \
            struct timespec _a = a;                                     \
            struct timespec _b = b;                                     \
            (struct timespec)                                           \
            { .tv_sec = (_a.tv_sec - _b.tv_sec) - (_a.tv_nsec < _b.tv_nsec ? 1 : 0), \
                    .tv_nsec = (1000000000 + (_a.tv_nsec - _b.tv_nsec)) % 1000000000 }; \
        })

    #define ts_imult(a, v)                                              \
        ({                                                              \
            struct timespec _a = a;                                     \
            __auto_type _v = v;                                         \
            (struct timespec)                                           \
            { .tv_sec = (_a.tv_sec * _v) + ((_a.tv_nsec * _v) / 1000000000), \
                    .tv_nsec = (_a.tv_nsec * _v) % 1000000000 };        \
        })

    #define ts_fms(v)                                                   \
        ({                                                              \
            __auto_type _v = v;                                         \
            (struct timespec)                                           \
            { .tv_sec = _v / 1000, .tv_nsec = (_v * 1000000) % 1000000000 }; \
        })
}

@ {
    #ifndef TIME_UTC
    #define TIME_UTC_MANUAL_DEF
    #define TIME_UTC 1
    #endif
}

/* MinGW installations can trip up on `timespec_get` since its a newer standard. */
#if defined(_WIN32) && defined(TIME_UTC_MANUAL_DEF)
@ int timespec_get(struct timespec* ts, int base) {
    if (base == TIME_UTC) {
        LARGE_INTEGER count;
        /* Apparently XP and later never returns zero, but who knows if someone needs ME compatibility! */
        if (!QueryPerformanceCounter(&count)) {
            ts->tv_nsec = 0;
            ts->tv_sec  = 0;
            return 0;
        }
        ts->tv_nsec = (count.QuadPart % 1000000) * 1000;
        ts->tv_sec = count.QuadPart / (uint64_t) 1000000;
        return TIME_UTC;
    }
    ts->tv_nsec = 0;
    ts->tv_sec  = 0;
    return 0;
}
#endif

@ int sleep_blocking(int ms) {
    #ifdef _WIN32
    Sleep(ms);
    return 0;
    #else
    struct timespec ts;
    int res;
    
    if (ms < 0) {
        errno = EINVAL;
        return -1;
    }
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000;

    do {
        res = nanosleep(&ts, &ts);
    } while (res && errno == EINTR);

    return res;
    #endif
}

#define async_queue_invoke(q, storage)            \
    ({                                            \
        __auto_type _q = q;                       \
        cmd_queue_push(&_q->queue, storage);      \
        async_cond_signal(&_q->cond);             \
    })

@ {
    #ifdef _WIN32
    #define AFD_READ GENERIC_READ
    #define AFD_WRITE GENERIC_WRITE
    #define AFD_CREATE 0x00010000
    #define AFD_RW (GENERIC_READ | GENERIC_WRITE)
    #define AFD_APPEND 0x0002000
    #define AFD_TRUNCATE 0x00040000
    #define AFD_ERR_INVALID_ARG (ASYNC_ERR_OFFSET + ERROR_BAD_ARGUMENTS)
    #define AFD_SEEK_SET FILE_BEGIN
    #define AFD_SEEK_CUR FILE_CURRENT
    #define AFD_SEEK_END FILE_END
    #else
    #define AFD_READ O_RDONLY
    #define AFD_WRITE O_WRONLY
    #define AFD_CREATE O_CREAT
    #define AFD_RW O_RDWR
    #define AFD_APPEND O_APPEND
    #define AFD_TRUNCATE O_TRUNC
    #define AFD_ERR_INVALID_ARG (ASYNC_ERR_OFFSET + EINVAL)
    #define AFD_UNIX_MODE 0644
    #define AFD_SEEK_SET SEEK_SET
    #define AFD_SEEK_CUR SEEK_CUR
    #define AFD_SEEK_END SEEK_END
    #endif
}

#define cmd_flag_future(cmd, value) ct_flag_future(cmd->task, &cmd->future, value)

#define AC_OFFLOAD 1
static inline void AC_OFFLOAD_handler(async_cmd* cmd) {
    ctx_swap(&cmd->data.offload.ctx, &cmd->task->context);
    ct_return_offload(cmd->task->sched, cmd->task);
}
@ {
    #define ct_sync(_cmd)                                               \
        ({                                                              \
            __auto_type _cmd_v = _cmd;                                  \
            ctx_swap(&(_cmd_v)->task->context, &(_cmd_v)->data.offload.ctx); \
        })
    
    #define CT_OFFLOAD(cmd)                                             \
        __auto_type _cmd = cmd;                                         \
        for (bool _i = ct_offload_f(_cmd); _i; ({ ct_sync(_cmd); _i = false; }))
}
@ void async_offload_f(ct_task* task) {
    async_cmd* cmd = (async_cmd*) task->store;
    cmd->id = AC_OFFLOAD;
    cmd->task = task;
    async_queue_invoke(&queue_storage, cmd);
}
@ bool ct_offload_f(async_cmd* cmd) {
    ct_current_task->store = cmd;
    ct_resume_offload(ct_current, ct_current_task);
    ct_iter(ct_current, ct_current_task, true);
    return true;
}

#define AC_FILE_OPEN 2
static inline void AC_FILE_OPEN_handler(async_cmd* cmd) {
    #ifdef _WIN32
    size_t size = strlen(cmd->data.fs.path);
    char16_t buf[MAX_PATH];
    utf8_to_utf16(cmd->data.fs.path, size, buf, MAX_PATH);
    DWORD create_d = OPEN_EXISTING;
    bool append = false;
    
    if (cmd->data.fs.flags & AFD_TRUNCATE) {
        cmd->data.fs.flags ^= AFD_TRUNCATE;
        if (cmd->data.fs.flags & AFD_CREATE)
            create_d = CREATE_ALWAYS;
        else
            create_d = TRUNCATE_EXISTING;
    } else if (cmd->data.fs.flags & AFD_CREATE) {
        cmd->data.fs.flags ^= AFD_CREATE;
        create_d = OPEN_ALWAYS;
    }
    
    if (cmd->data.fs.flags & AFD_APPEND) {
        cmd->data.fs.flags ^= AFD_APPEND;
        append = true;
    }
    
    HANDLE h = CreateFileW(buf, cmd->data.fs.flags, FILE_SHARE_WRITE | FILE_SHARE_READ,
                           NULL, create_d, FILE_ATTRIBUTE_NORMAL, NULL);
    
    if (h != INVALID_HANDLE_VALUE) {
        if (append) {
            if (SetFilePointer(h, 0, NULL, FILE_END) == INVALID_SET_FILE_POINTER)
                cmd_flag_future(cmd, ASYNC_ERR_OFFSET + GetLastError());
        }
        *(cmd->data.fs.storage) = h;
        cmd_flag_future(cmd, FUTURE_OK);
    } else cmd_flag_future(cmd, ASYNC_ERR_OFFSET + GetLastError());
    #else
    
    /* O_TRUNC and O_CREAT when the specified file does not exist results in UB
       as per the O_TRUNC documention. In practise, this doesn't fail. */
    
    //todo: handle if ((cmd->data.fs.flags & AFD_TRUNCATE) && (cmd->data.fs.flags & AFD_CREATE)) { ... }
    
    int fd = open(cmd->data.fs.path, cmd->data.fs.flags, AFD_UNIX_MODE);
    if (fd != -1) {
        *(cmd->data.fs.storage) = fd;
        cmd_flag_future(cmd, FUTURE_OK);
    } else cmd_flag_future(cmd, ASYNC_ERR_OFFSET + errno);
    #endif
}
@ void async_file_open(async_cmd* cmd, async_fd* file, const char* path, int flags) {
    ct_task*  task       = ct_current_task;
    cmd->future          = FUTURE_INIT;
    cmd->id              = AC_FILE_OPEN;
    cmd->task            = task;
    cmd->data.fs.path    = path;
    cmd->data.fs.flags   = flags;
    cmd->data.fs.storage = file;
    async_queue_invoke(&queue_storage, cmd);
}

#define AC_FILE_CLOSE 3
static inline void AC_FILE_CLOSE_handler(async_cmd* cmd) {
    #ifdef _WIN32
    if (CloseHandle(*cmd->data.fs.storage) != 0) {
        *(cmd->data.fs.storage) = INVALID_HANDLE_VALUE;
        cmd_flag_future(cmd, FUTURE_OK); 
    } else cmd_flag_future(cmd, ASYNC_ERR_OFFSET + GetLastError());
    #else
    if (close(*cmd->data.fs.storage) == 0) {
        *(cmd->data.fs.storage) = -1;
        cmd_flag_future(cmd, FUTURE_OK); 
    } else cmd_flag_future(cmd, ASYNC_ERR_OFFSET + errno);
    #endif
}
@ void async_file_close(async_cmd* cmd, async_fd* file) {
    ct_task*  task       = ct_current_task;
    cmd->future          = FUTURE_INIT;
    cmd->id              = AC_FILE_CLOSE;
    cmd->task            = task;
    cmd->data.fs.storage = file;
    async_queue_invoke(&queue_storage, cmd);
}

#define AC_FILE_READ 4
static inline void AC_FILE_READ_handler(async_cmd* cmd) {
    #ifdef _WIN32
    DWORD word;
    if (ReadFile(cmd->data.io.fd, cmd->data.io.ptr, (DWORD) cmd->data.io.sz, &word, NULL)) {
        *(cmd->data.io.ret) = (size_t) word;
        cmd_flag_future(cmd, FUTURE_OK);
    } else cmd_flag_future(cmd, ASYNC_ERR_OFFSET + GetLastError());
    #else
    ssize_t ret = read(cmd->data.io.fd, cmd->data.io.ptr, cmd->data.io.sz);
    if (ret >= 0) {
        *(cmd->data.io.ret) = (size_t) ret;
        cmd_flag_future(cmd, FUTURE_OK);
    } else cmd_flag_future(cmd, ASYNC_ERR_OFFSET + errno);
    #endif
}
@ void async_file_read(async_cmd* cmd, async_fd file, void* dest, size_t bytes, size_t* read) {
    ct_task*  task      = ct_current_task;
    cmd->future         = FUTURE_INIT;
    cmd->id             = AC_FILE_READ;
    cmd->task           = task;
    cmd->data.io.fd     = file;
    cmd->data.io.ptr    = dest;
    cmd->data.io.sz     = bytes;
    cmd->data.io.ret    = read;
    async_queue_invoke(&queue_storage, cmd);
}

#define AC_FILE_WRITE 5
static inline void AC_FILE_WRITE_handler(async_cmd* cmd) {
    #ifdef _WIN32
    DWORD word;
    if (WriteFile(cmd->data.io.fd, cmd->data.io.ptr, (DWORD) cmd->data.io.sz, &word, NULL)) {
        *(cmd->data.io.ret) = (size_t) word;
        cmd_flag_future(cmd, FUTURE_OK);
    } else cmd_flag_future(cmd, ASYNC_ERR_OFFSET + GetLastError());
    #else
    ssize_t ret = write(cmd->data.io.fd, cmd->data.io.ptr, cmd->data.io.sz);
    if (ret >= 0) {
        *(cmd->data.io.ret) = (size_t) ret;
        cmd_flag_future(cmd, FUTURE_OK);
    } else cmd_flag_future(cmd, ASYNC_ERR_OFFSET + errno);
    #endif
}
@ void async_file_write(async_cmd* cmd, async_fd file,
                        const void* src, size_t bytes, size_t* written) {
    ct_task* task       = ct_current_task;
    cmd->future         = FUTURE_INIT;
    cmd->id             = AC_FILE_WRITE;
    cmd->task           = task;
    cmd->data.io.fd     = file;
    cmd->data.io.ptr    = (void*) src;
    cmd->data.io.sz     = bytes;
    cmd->data.io.ret    = written;
    async_queue_invoke(&queue_storage, cmd);
}

#define AC_DIR_CREATE 6
static inline void AC_DIR_CREATE_handler(async_cmd* cmd) {
    #ifdef _WIN32
    size_t size = strlen(cmd->data.fs.path);
    char16_t buf[MAX_PATH];
    utf8_to_utf16(cmd->data.fs.path, size, buf, MAX_PATH);
    if (CreateDirectoryW(buf, NULL)) {
        cmd_flag_future(cmd, FUTURE_OK);
    } else cmd_flag_future(cmd, ASYNC_ERR_OFFSET + GetLastError());
    #else
    if (!mkdir(cmd->data.fs.path, AFD_UNIX_MODE)) {
        cmd_flag_future(cmd, FUTURE_OK);
    } else cmd_flag_future(cmd, ASYNC_ERR_OFFSET + errno);
    #endif
}
@ void async_dir_create(async_cmd* cmd, const char* path) {
    ct_task* task       = ct_current_task;
    cmd->future         = FUTURE_INIT;
    cmd->id             = AC_DIR_CREATE;
    cmd->task           = task;
    cmd->data.fs.path   = path;
    async_queue_invoke(&queue_storage, cmd);
}

#define AC_DIR_REMOVE 7
static inline void AC_DIR_REMOVE_handler(async_cmd* cmd) {
    #ifdef _WIN32
    size_t size = strlen(cmd->data.fs.path);
    char16_t buf[MAX_PATH];
    utf8_to_utf16(cmd->data.fs.path, size, buf, MAX_PATH);
    if (RemoveDirectoryW(buf)) {
        cmd_flag_future(cmd, FUTURE_OK);
    } else cmd_flag_future(cmd, ASYNC_ERR_OFFSET + GetLastError());
    #else
    if (!rmdir(cmd->data.fs.path)) {
        cmd_flag_future(cmd, FUTURE_OK);
    } else cmd_flag_future(cmd, ASYNC_ERR_OFFSET + errno);
    #endif
}
@ void async_dir_remove(async_cmd* cmd, const char* path) {
    ct_task* task       = ct_current_task;
    cmd->future         = FUTURE_INIT;
    cmd->id             = AC_DIR_REMOVE;
    cmd->task           = task;
    cmd->data.fs.path   = path;
    async_queue_invoke(&queue_storage, cmd);
}

#define AC_FILE_REMOVE 8
static inline void AC_FILE_REMOVE_handler(async_cmd* cmd) {
    #ifdef _WIN32
    size_t size = strlen(cmd->data.fs.path);
    char16_t buf[MAX_PATH];
    utf8_to_utf16(cmd->data.fs.path, size, buf, MAX_PATH);
    if (DeleteFileW(buf)) {
        cmd_flag_future(cmd, FUTURE_OK);
    } else cmd_flag_future(cmd, ASYNC_ERR_OFFSET + GetLastError());
    #else
    if (!unlink(cmd->data.fs.path)) {
        cmd_flag_future(cmd, FUTURE_OK);
    } else cmd_flag_future(cmd, ASYNC_ERR_OFFSET + errno);
    #endif
}
@ void async_file_remove(async_cmd* cmd, const char* path) {
    ct_task* task       = ct_current_task;
    cmd->future         = FUTURE_INIT;
    cmd->id             = AC_FILE_REMOVE;
    cmd->task           = task;
    cmd->data.fs.path   = path;
    async_queue_invoke(&queue_storage, cmd);
}

#define AC_FILE_SEEK 9
static inline void AC_FILE_SEEK_handler(async_cmd* cmd) {
    #ifdef _WIN32
    if (SetFilePointer(cmd->data.io.fd, (off_t) cmd->data.io.extra, NULL, (DWORD) cmd->data.io.flags)
        != INVALID_SET_FILE_POINTER) {
        cmd_flag_future(cmd, FUTURE_OK);
    } else cmd_flag_future(cmd, ASYNC_ERR_OFFSET + GetLastError());
    #else
    if (lseek(cmd->data.io.fd, (off_t) cmd->data.io.extra, cmd->data.io.flags) != (off_t) -1) {
        cmd_flag_future(cmd, FUTURE_OK);
    } else cmd_flag_future(cmd, ASYNC_ERR_OFFSET + errno);
    #endif
}
@ void async_file_seek(async_cmd* cmd, async_fd file, int offset, int whence) {
    ct_task*  task      = ct_current_task;
    cmd->future         = FUTURE_INIT;
    cmd->id             = AC_FILE_SEEK;
    cmd->task           = task;
    cmd->data.io.fd     = file;
    cmd->data.io.extra  = offset;
    cmd->data.io.flags  = whence;
    async_queue_invoke(&queue_storage, cmd);
}

#define AC_HANDLER(id, command)                 \
    case id: {                                  \
        async_cmd* _c = command;                \
        ct_current = command->task->sched;      \
        id##_handler(_c);                       \
        ct_current = NULL;                      \
        break;                                  \
    }

#define QUEUE_TIMEDWAIT_DURATION_MS 50
async_entry_sig static async_entry_retval queue_entry(async_queue_member* self) {
    cmd_queue*   q      = &self->parent->queue;
    async_mutex* pmutex = &self->parent->mutex;
    async_cond*  pcond  = &self->parent->cond;
    async_cmd*   cmd;
    for (;;) {
        /* Locks are used to access a MPSC queue due to performance constraints only being
           critical for synchronous code; we don't reallycare if there is a lack in worker
           thread responsiveness, nor should this lock be particularily contended.
         
           As long as much asynchronous code is long lived (including blocking operations),
           this pattern may be the most performant due to the speed of the underlying queue
           and its wait-free implementation for producers. */
        async_mutex_lock(pmutex);
        while ((cmd = cmd_queue_pop(q)) == NULL) {
            /*
              A timedwait is used due to non-locked signals being issued from the scheduler thread.
              Missed signals imply heavily loaded worker threads anyway, due to multiple workers
              waiting on the same mutex (every single worker thread would have to be in this lock
              region, yet not actually yielded!)
            */
            struct timespec ts;
            timespec_get(&ts, TIME_UTC);
            ts.tv_nsec += QUEUE_TIMEDWAIT_DURATION_MS * 1000000;
            async_cond_timedwait(pcond, pmutex, &ts);
            if (!__atomic_load_n(&self->running, __ATOMIC_SEQ_CST)) {
                async_mutex_unlock(pmutex);
                goto ret;
            }
        }
        async_mutex_unlock(pmutex);
        switch (cmd->id) {
            AC_HANDLER(AC_OFFLOAD,     cmd);
            AC_HANDLER(AC_FILE_OPEN,   cmd);
            AC_HANDLER(AC_FILE_CLOSE,  cmd);
            AC_HANDLER(AC_FILE_READ,   cmd);
            AC_HANDLER(AC_FILE_WRITE,  cmd);
            AC_HANDLER(AC_DIR_CREATE,  cmd);
            AC_HANDLER(AC_FILE_REMOVE, cmd);
            AC_HANDLER(AC_DIR_REMOVE,  cmd);
            AC_HANDLER(AC_FILE_SEEK,   cmd);
            default: {
                fprintf(stderr, "async_queue: invalid command id: %d\n", (int) cmd->id);
                exit(EXIT_FAILURE);
                break;
            }
        }
    }
ret:
    async_thread_return();
}

/*
  The above asynchronous IO functions are also wrapped via Lua in an API-compatible manner;
  the following functions replace io.* and file:* functions, seamlessly allowing traditional
  lua file operations to never block the main thread.

  A downside is paid for these implementations: because these functions often contain many
  async commands submitted iteratively, rather than offloading and performing them together
  in the same command, responsiveness is poor. Multiple scheduler cycles are often needed to
  complete a single lua async call.
 */

/* Helper functions for Lua implementations */
static void readblock(lua_State* L, async_cmd* cmd, async_fd fd, char* buf, size_t sz, size_t* read_store) {
    size_t read = 0, offset = 0;
    do {
        async_file_read(cmd, fd, buf + offset, sz - offset, &read);
        ct_complete(&cmd->future);
        offset += read;
        if (cmd->future != FUTURE_OK) {
            const char* strerror = future_str(cmd->future);
            lua_pushfstring(L, "`async_fd:read` failure: %s", strerror);
            future_str_free(strerror);
            lua_error(L);
        }
    } while (read != 0 && offset < sz);
    *read_store = offset;
}

static void writeblock(lua_State* L, async_cmd* cmd, async_fd fd, const char* buf,
                       size_t sz, size_t* write_store) {
    size_t write = 0, offset = 0;
    do {
        async_file_write(cmd, fd, buf + offset, sz - offset, &write);
        ct_complete(&cmd->future);
        offset += write;
        if (cmd->future != FUTURE_OK) {
            const char* strerror = future_str(cmd->future);
            lua_pushfstring(L, "`async_fd:write` failure: %s", strerror);
            future_str_free(strerror);
            lua_error(L);
        }
    } while (write != 0 && offset < sz);
    if (write_store != NULL)
        *write_store = offset;
}

static void seekcur(lua_State* L, async_cmd* cmd, async_fd fd, int by) {
    async_file_seek(cmd, fd, by, AFD_SEEK_CUR);
    ct_complete(&cmd->future);
    if (cmd->future != FUTURE_OK) {
        const char* strerror = future_str(cmd->future);
        lua_pushfstring(L, "`async_fd:read` failure during seek (reset): %s", strerror);
        future_str_free(strerror);
        lua_error(L);
    }
}

LUA_CONSTRUCTOR(async_fd, 0) {}
#ifndef _WIN32
LUA_GETTER_SELF_NUMBER(async_fd, internal_handle)
LUA_SETTER_SELF_NUMBER(async_fd, internal_handle)
#else
LUA_GETTER_SELF_POINTER(async_fd, internal_handle)
LUA_SETTER_SELF_POINTER(async_fd, internal_handle)
#endif
LUA_METHOD(async_fd, read) {
    int top_store = lua_gettop(L);
    for (int t = 2; t <= top_store; ++t) {
        if (lua_isstring(L, t)) {
            const char* arg = lua_tostring(L, t);
            if ((strlen(arg) < 2) || arg[0] != '*')
                return luaL_error(L, "Invalid format for `async_fd:read`: %s", arg);
            switch (arg[1]) {
                case 'n': {
                    async_cmd cmd;
                    char buf[17];
                    char* end;
                    size_t read, rollback;
                    readblock(L, &cmd, *self, buf, sizeof(buf) - 1, &read);
                    buf[read] = '\0';
                    lua_Number value = (lua_Number) strtod(buf, &end);
                    rollback = read - ((intptr_t) end - (intptr_t) (char*) buf);
                    seekcur(L, &cmd, *self, -rollback);
                    lua_pushnumber(L, value);
                } break;
                case 'a': {
                    async_cmd cmd;
                    size_t read, buf_sz = 4096, alloc_sz = 0;
                    char* buf = NULL;
                    do {
                        alloc_sz += buf_sz;
                        buf = ((buf == NULL) ? malloc(alloc_sz) : realloc(buf, alloc_sz));
                        readblock(L, &cmd, *self, buf + (alloc_sz - buf_sz), buf_sz, &read);
                    } while (read == buf_sz);
                    size_t len = alloc_sz - (buf_sz - read);
                    lua_pushlstring(L, buf, len);
                    free(buf);
                } break;
                case 'l': {
                    async_cmd cmd;
                    size_t read, buf_sz = 4096, alloc_sz = 0, newline, rollback;
                    char* buf = NULL;
                    do {
                        alloc_sz += buf_sz;
                        buf = ((buf == NULL) ? malloc(alloc_sz) : realloc(buf, alloc_sz));
                        readblock(L, &cmd, *self, buf + (alloc_sz - buf_sz), buf_sz, &read);
                        for (int t = 0; t < read; ++t) {
                            char at = *(buf + (alloc_sz - buf_sz) + t);
                            if (at == '\n') {
                                newline = (alloc_sz - buf_sz) + t;
                                rollback = (read - t) - 1;
                                goto newline_found;
                            }
                        }
                    } while (read == buf_sz);
                    size_t len = alloc_sz - (buf_sz - read);
                    if (len == 0)
                        lua_pushnil(L);
                    else
                        lua_pushlstring(L, buf, len);
                    free(buf);
                    break;
                newline_found:
                    lua_pushlstring(L, buf, newline);
                    free(buf);
                    seekcur(L, &cmd, *self, -rollback);
                } break;
                default:
                    return luaL_error(L, "Invalid format for `async_fd:read`: %s", arg);
            }
        } else if (lua_isnumber(L, t)) {
            lua_Integer i = lua_tointeger(L, t);
            if (i <= 0)
                return luaL_error(L, "Invalid argument for `async_fd:read`: negative or zero integer");
            size_t n = (size_t) i;
            async_cmd cmd;
            char* buf = malloc(n);
            size_t read;
            readblock(L, &cmd, *self, buf, sizeof(buf), &read);
            if (read == 0)
                lua_pushnil(L);
            else
                lua_pushlstring(L, buf, read);
        }
    }
    return top_store - 1;
}

LUA_METHOD(async_fd, write) {
    int top_store = lua_gettop(L);
    for (int t = 2; t <= top_store; ++t) {
        if (lua_isstring(L, t) || lua_isnumber(L, t)) {
            async_cmd cmd;
            size_t len;
            const char* str = lua_tolstring(L, t, &len);
            writeblock(L, &cmd, *self, str, len, NULL);
        } else {
            return luaL_error(L, "Invalid argument for `async_fd:write`: invalid type");
        }
    }
    return 0;
}

LUA_METHOD(async_fd, close) {
    async_cmd cmd;
    async_file_close(&cmd, self);
    ct_complete(&cmd.future);
    if (cmd.future != FUTURE_OK) {
        const char* strerror = future_str(cmd.future);
        lua_pushfstring(L, "`async_fd:close` failure: %s", strerror);
        future_str_free(strerror);
        lua_error(L);
    }
    return 0;
}

static int lua_async_fd_open(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    char mode[3] = { 'r', '\0', '\0' };
    if (lua_gettop(L) >= 2) {
        const char* arg = luaL_checkstring(L, 2);
        if (strlen(arg) < 1)
            return luaL_error(L, "Invalid mode for `async_fd_open`: %s", arg);
        mode[0] = arg[0];
        mode[1] = arg[1];
        if ((arg[1] != '\0') && (arg[2] != '\0'))
            return luaL_error(L, "Invalid mode for `async_fd_open`: %s", arg);
        lua_pop(L, 1); /* pop second argument for stack consistency */
    }
    int flags = 0;
    switch (mode[0]) {
        case 'r':
            switch (mode[1]) {
                case '\0': flags = AFD_READ; break;
                case '+':  flags = AFD_RW;   break;
                default: goto invalid;
            }
            break;
        case 'w':
            switch (mode[1]) {
                case '\0': flags = AFD_WRITE | AFD_TRUNCATE | AFD_CREATE; break;
                case '+':  flags = AFD_RW    | AFD_TRUNCATE | AFD_CREATE; break;
                default: goto invalid;
            }
            break;
        case 'a':
            switch (mode[1]) {
                case '\0': flags = AFD_APPEND | AFD_WRITE | AFD_CREATE; break;
                case '+':  flags = AFD_APPEND | AFD_RW    | AFD_CREATE; break;
                default: goto invalid;
            }
            break;
        default: {
            invalid:
            return luaL_error(L, "Invalid mode for `async_fd_open`: %s", mode);
            break;
        }
    }
    async_cmd cmd;
    async_fd file;
    async_file_open(&cmd, &file, path, flags);
    ct_complete(&cmd.future);
    if (cmd.future != FUTURE_OK) {
        const char* strerror = future_str(cmd.future);
        lua_pushfstring(L, "`async_fd_open` failure: %s", strerror);
        future_str_free(strerror);
        lua_error(L);
    }
    lua_pushcfunction(L, _async_fd__constructor);
    lua_call(L, 0, 1);
    async_fd* storage = lua_touserdata(L, -1);
    *storage = file;
    return 1;
}
LUA_REGISTER_FUNCTION(lua_async_fd_open, "@c_async_fd_open");

static int lua_async_file_remove(lua_State* L) {
    async_cmd cmd;
    async_file_remove(&cmd, luaL_checkstring(L, 1));
    ct_complete(&cmd.future);
    if (cmd.future != FUTURE_OK) {
        const char* strerror = future_str(cmd.future);
        lua_pushfstring(L, "`async_file_remove` failure: %s", strerror);
        future_str_free(strerror);
        return lua_error(L);
    }
    return 0;
}
LUA_REGISTER_FUNCTION(lua_async_file_remove, "@c_async_file_remove");

static int lua_async_dir_remove(lua_State* L) {
    async_cmd cmd;
    async_dir_remove(&cmd, luaL_checkstring(L, 1));
    ct_complete(&cmd.future);
    if (cmd.future != FUTURE_OK) {
        const char* strerror = future_str(cmd.future);
        lua_pushfstring(L, "`async_dir_remove` failure: %s", strerror);
        future_str_free(strerror);
        return lua_error(L);
    }
    return 0;
}
LUA_REGISTER_FUNCTION(lua_async_dir_remove, "@c_async_dir_remove");

static int lua_async_dir_create(lua_State* L) {
    async_cmd cmd;
    async_dir_create(&cmd, luaL_checkstring(L, 1));
    ct_complete(&cmd.future);
    if (cmd.future != FUTURE_OK) {
        const char* strerror = future_str(cmd.future);
        lua_pushfstring(L, "`async_dir_create` failure: %s", strerror);
        future_str_free(strerror);
        return lua_error(L);
    }
    return 0;
}
LUA_REGISTER_FUNCTION(lua_async_dir_create, "@c_async_dir_create");

__attribute__((constructor)) static void async_queue_create(void) {
    async_queue* queue = &queue_storage;
    *queue = (async_queue) {
        .members = {},
        .write   = 0
    };
    async_mutex_init(&queue->mutex);
    async_cond_init(&queue->cond);
    cmd_queue_init(&queue->queue);
    for (int t = 0; t < ASYNC_QUEUE_NUM_THREADS; ++t) {
        queue->members[t].parent = queue;
        queue->members[t].running = true;
        async_thread_create(&queue->members[t].thread, (async_thread_entry) queue_entry, &queue->members[t]);
        async_thread_name(&queue->members[t].thread, "worker thread");
    }
}

__attribute__((destructor)) static void async_queue_destroy(void) {
    async_queue* self = &queue_storage;
    for (int t = 0; t < ASYNC_QUEUE_NUM_THREADS; ++t) {
        __atomic_store_n(&self->members[t].running, false, __ATOMIC_SEQ_CST);
    }
    async_cond_broadcast(&self->cond);
    for (int t = 0; t < ASYNC_QUEUE_NUM_THREADS; ++t) {
        async_thread_join(&self->members[t].thread);
    }
    
    cmd_queue_destroy(&self->queue);
}
