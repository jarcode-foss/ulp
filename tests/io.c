#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#include <assert.h>

#include <luabase.h>
#include <ct.h>

static ct_scheduler frame_scheduler;

static ct_task* main_task;
static bool task_completed = false;
// static future* ref;

#define chk(C, ...)                             \
    do {                                        \
        __VA_ARGS__;                            \
        ct_complete(&C.future);                 \
        debug_errfail(C.future);                \
    } while (0)

static void main_task_f(ct_arg task) {
    CT_BEGIN(task) {
        printf("entered main task\n");
        async_cmd cmd;
        async_fd fd;
        chk(cmd, async_file_open(&cmd, &fd, "test.txt", AFD_RW | AFD_CREATE));
        const char* str = "Hello World!";
        size_t written, read = -1, t, sz = strlen(str) * sizeof(char);
        for (t = 0; t < sz; t += written) {
            chk(cmd, async_file_write(&cmd, fd, str + t, sz, &written));
        }
        chk(cmd, async_file_close(&cmd, &fd));
        chk(cmd, async_file_open(&cmd, &fd, "test.txt", AFD_RW));
        
        printf("Reading file...\n");
        
        char buf[64];
        for (t = 0; t < sizeof(buf) && read != 0; t += read) {
            chk(cmd, async_file_read(&cmd, fd, buf, sizeof(buf), &read));
        }
        if (strcmp(buf, str)) {
            fprintf(stderr, "String read back from file failed to match, got '%s' instead of expected '%s'\n",
                    buf, str);
            abort();
        }
        printf("Removing file...\n");
        chk(cmd, async_file_close(&cmd, &fd)); /* all descriptors must be closed on windows before removal */
        chk(cmd, async_file_remove(&cmd, "test.txt"));
        printf("File removed!\n");
        task_completed = true;
    }
}

int main(int argc, char** argv) {
    luaA_entry();
    ct_init(&frame_scheduler);
    main_task = schedule(&frame_scheduler, main_task_f, CT_LUA, 0);
    for (int t = 0; t < 10; ++t) {
        ct_enter(&frame_scheduler);
        sleep_blocking(50);
    }
    assert(task_completed);
    printf("returned from scheduler\n");
    return EXIT_SUCCESS;
}
