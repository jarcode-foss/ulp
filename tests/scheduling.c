
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#include <assert.h>

#include <luabase.h>
#include <ct.h>

#include <unistd.h>

/* Test for basic synchronous scheduler functionality */

static ct_scheduler frame_scheduler;

static ct_task * t1, * t2;
static future* ref;

static int val = 0;

static void async_thing(future* storage) {
    *storage = FUTURE_WAITING;
    ref = storage;
}

static void my_other_task(ct_arg task) {
    CT_BEGIN(task) {
        printf("t2\n");
        assert(val == 1);
        val++;
        ct_resume(t1);
        ct_yield();
        assert(val == 3);
        val++;
        ct_flag_future(t1, ref, FUTURE_OK);
        printf("flagged future!\n");
    }
}

static void my_task(ct_arg task) {
    CT_BEGIN(task) {
        printf("t1\n");
        assert(val == 0);
        val++;
        t2 = schedule(&frame_scheduler, my_other_task, CT_LUA, 0);
        ct_yield();
        assert(val == 2);
        val++;
        ct_resume(t2);
        ct_do(async_thing(&_));
        assert(val == 4);
        val++;
        printf("returned from async thing!\n");

        async_cmd cmd;
        CT_OFFLOAD(&cmd) {
            printf("this is being done asynchronously!\n");
        }
        
        printf("this is being done synchronously!\n");
    }
}


static void repeat_task(ct_arg task) {
    CT_BEGIN(task) {
        printf("this should repeat every second\n");
    }
}

int main(int argc, char** argv) {
    luaA_entry();
    ct_init(&frame_scheduler);
    t1 = schedule(&frame_scheduler, my_task, CT_LUA, 0);
    schedule(&frame_scheduler, repeat_task, CT_REPEAT | CT_TIMED, 1000);
    for (int t = 0; t < 4; ++t) {
        ct_enter(&frame_scheduler);
        sleep_blocking(50);
    }
    // sleep_blocking(1000);
    assert(val == 5);
    printf("returned from scheduler\n");
    return EXIT_SUCCESS;
}
