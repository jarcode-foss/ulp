#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#include <assert.h>

#include <luabase.h>
#include <ct.h>

static ct_scheduler frame_scheduler;

static ct_task* main_task;

static char* lua_source =
    "local foo_var = C.foo {\n"
    "    a = 42,\n"
    "    b = \"foobar\"\n"
    "}\n"
    "print(foo_var:get_a())\n"
    "print(foo_var:get_b())\n"
    "schedule(function() print \"This is from a scheduled task in Lua!\" end, 0)\n"
    "local file = io.open(\"ltest.txt\", \"w+\")\n"
    "file:write(\"foo\\nbiz\\nbar\\nbash\\n\")\n"
    "file:close()\n"
    "file = io.open(\"ltest.txt\", \"r\")\n"
    "print(\"fd handle: \" .. tostring(file:get_internal_handle()))\n"
    "print \"----------\"\n"
    "for line in file:lines() do\n"
    "print(line)\n"
    "end\n"
    "file:close()\n"
    "io.file_remove(\"ltest.txt\")";

typedef struct {
    int a;
    char* b;
} foo;

LUA_CONSTRUCTOR(foo, {
        .a = 1,
        .b = strdup("Hello World!")
    }) {
    printf("C: struct constructed (a = %d, b = \"%s\")\n", self->a, self->b);
}

LUA_GETTER_NUMBER(foo, a);
LUA_SETTER_NUMBER(foo, a);
LUA_GETTER_STRING(foo, b);
LUA_SETTER_STRING(foo, b);

static bool completed = false;
static void main_task_f(ct_arg task) {
    CT_BEGIN(task) {
        luaL_loadstring(ct_lua_state, lua_source);
        if (!luaA_calltop(ct_lua_state))
            abort();
        completed = true;
    }
}

int main(int argc, char** argv) {
    luaA_entry();
    ct_init(&frame_scheduler);
    main_task = schedule(&frame_scheduler, main_task_f, CT_LUA, 0);
    while (!completed) {
        ct_enter(&frame_scheduler);
        sleep_blocking(5);
    }
    printf("returned from scheduler\n");
}
