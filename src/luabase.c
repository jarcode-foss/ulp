#include <luabase.h>

/* Dynamically include lua headers and utilities, regardless of implementation/version */
@ {
    #include <stdlib.h>
    #include <stdio.h>
    #include <stdbool.h>
    #include <string.h>
    #define _DECORATE(x) <x>
    #define _MAKE_PATH(root, file) _DECORATE(root/file)
    #define _INCLUDE(file) _MAKE_PATH(APP_LUA_INCLUDE, file)
    #include _INCLUDE(lua.h)
    #include _INCLUDE(lauxlib.h)
    #include _INCLUDE(lualib.h)
    #undef _DECORATE
    #undef _MAKE_PATH
    #undef _INCLUDE
    
    #define LUA_REGISTER_FUNCTION(function, str)                        \
        __attribute__((constructor)) static void function##__init(void) { \
            luaA_register(function, str);  \
        }

    #define LUA_METHOD(type, name)                                      \
        static int _##type##_##name(lua_State* L);                      \
        static inline int _##type##__f_##name(lua_State* L, type* self); \
        LUA_REGISTER_FUNCTION(_##type##_##name, "@c_" #type "." #name); \
        static int _##type##_##name(lua_State* L) {                     \
            return _##type##__f_##name(L, (type*) luaL_checkudata(L, 1, "c_" #type)); \
        }                                                               \
        static inline int _##type##__f_##name(lua_State* L, type* self)
    
    #define LUA_CONSTRUCTOR(type, ...)                                  \
        static void _##type##__new(lua_State* L, type* self);           \
        static int _##type##__constructor(lua_State* L) {               \
            type* self = lua_newuserdata(L, sizeof(type));              \
            *self = (type) __VA_ARGS__;                                 \
            luaA_constructor(L, "c_" #type);                            \
            _##type##__new(L, self);                                    \
            return 1;                                                   \
        }                                                               \
        LUA_REGISTER_FUNCTION(_##type##__constructor, "@c_" #type ".!"); \
        static void _##type##__new(lua_State* L, type* self)
    
    #define LUA_GETTER_STRING(type, field)                              \
        static int _##type##_get_##field(lua_State* L) {                \
            type* self = (type*) luaL_checkudata(L, 1, "c_" #type);     \
            lua_pushstring(L, self->field);                             \
            return 1;                                                   \
        }                                                               \
        LUA_REGISTER_FUNCTION(_##type##_get_##field, "@c_" #type ".get_" #field);
    
    /* IMPORTANT: this setter assumes the underlying string is heap managed! */
    #define LUA_SETTER_STRING(type, field)                              \
        static int _##type##_set_##field(lua_State* L) {                \
            type* self = (type*) luaL_checkudata(L, 1, "c_" #type);                  \
            char* value = strdup(luaL_checkstring(L, 2));               \
            if (self->field != NULL)                                    \
                free(self->field);                                      \
            self->field = value;                                        \
            return 0;                                                   \
        }                                                               \
        LUA_REGISTER_FUNCTION(_##type##_set_##field, "@c_" #type ".set_" #field);
    
    #define LUA_GETTER_NUMBER(type, field)                              \
        static int _##type##_get_##field(lua_State* L) {                \
            type* self = (type*) luaL_checkudata(L, 1, "c_" #type);     \
            lua_pushnumber(L, (lua_Number) self->field);                 \
            return 1;                                                   \
        }                                                               \
        LUA_REGISTER_FUNCTION(_##type##_get_##field, "@c_" #type ".get_" #field);
    
    #define LUA_SETTER_NUMBER(type, field)                              \
        static int _##type##_set_##field(lua_State* L) {                \
            type* self = (type*) luaL_checkudata(L, 1, "c_" #type);     \
            lua_Number value = luaL_checknumber(L, 2);                  \
            self->field = (typeof(self->field)) value;                  \
            return 0;                                                   \
        }                                                               \
        LUA_REGISTER_FUNCTION(_##type##_set_##field, "@c_" #type ".set_" #field);

        
    #define LUA_GETTER_SELF_NUMBER(type, field)                         \
        static int _##type##_get_##field(lua_State* L) {                \
            type* self = (type*) luaL_checkudata(L, 1, "c_" #type);     \
            lua_pushnumber(L, (lua_Number) *self);                      \
            return 1;                                                   \
        }                                                               \
        LUA_REGISTER_FUNCTION(_##type##_get_##field, "@c_" #type ".get_" #field);
    
    #define LUA_SETTER_SELF_NUMBER(type, field)                         \
        static int _##type##_set_##field(lua_State* L) {                \
            type* self = (type*) luaL_checkudata(L, 1, "c_" #type);     \
            lua_Number value = luaL_checknumber(L, 2);                  \
            *self = (type) value;                                       \
            return 0;                                                   \
        }                                                               \
        LUA_REGISTER_FUNCTION(_##type##_set_##field, "@c_" #type ".set_" #field);

    #define LUA_GETTER_SELF_POINTER(type, field)                        \
        static int _##type##_get_##field(lua_State* L) {                \
            type* self = (type*) luaL_checkudata(L, 1, "c_" #type);     \
            lua_pushlightuserdata(L, *self);                            \
            return 1;                                                   \
        }                                                               \
        LUA_REGISTER_FUNCTION(_##type##_get_##field, "@c_" #type ".get_" #field);
    
    #define LUA_SETTER_SELF_POINTER(type, field)                        \
        static int _##type##_set_##field(lua_State* L) {                \
            type* self = (type*) luaL_checkudata(L, 1, "c_" #type);     \
            if (!lua_islightuserdata(L, 2))                             \
                luaL_error(L, "expected lightuserdata");                \
            void* value = lua_touserdata(L, 2);                         \
            *self = (type) value;                                       \
            return 0;                                                   \
        }                                                               \
        LUA_REGISTER_FUNCTION(_##type##_set_##field, "@c_" #type ".set_" #field);
    
    #define LUA_GETTER_BOOLEAN(type, field)                              \
        static int _##type##_get_##field(lua_State* L) {                \
            type* self = (type*) luaL_checkudata(L, 1, "c_" #type);     \
            lua_pushboolean(L, (bool) self->field);                      \
            return 1;                                                   \
        }                                                               \
        LUA_REGISTER_FUNCTION(_##type##_get_##field, "@c_" #type ".get_" #field);
    
    #define LUA_SETTER_BOOLEAN(type, field)                              \
        static int _##type##_set_##field(lua_State* L) {                \
            type* self = (type*) luaL_checkudata(L, 1, "c_" #type);     \
            luaL_checktype(L, 2, LUA_TBOOLEAN);                         \
            self->field = (bool) lua_toboolean(L, 2);                   \
            return 0;                                                   \
        }                                                               \
        LUA_REGISTER_FUNCTION(_##type##_set_##field, "@c_" #type ".set_" #field);

    /* Performs a structure copy, not useful for references */
    #define LUA_SETTER_UDATA(type, field, udata_type)                   \
        static int _##type##_set_##field(lua_State* L) {                \
            type* self = (type*) luaL_checkudata(L, 1, "c_" #type);     \
            udata_type* value = (udata_type*) luaL_checkudata(L, 1, "c_" #udata_type); \
            memcpy(&self->field, value, sizeof(udata_type));            \
            return 0;                                                   \
        }                                                               \
        LUA_REGISTER_FUNCTION(_##type##_set_##field, "@c_" #type ".set_" #field);

    /* Iterates a table array of userdata types */
    #define LUA_STRUCT_ITER(L, idx, type, elem_name, ...)               \
        ({                                                              \
            for (int t = 1;; ++t) {                                     \
                lua_rawgeti(L, idx, t);                                 \
                if (lua_isnil(L, -1))                                   \
                    break;                                              \
                type* elem_name = (type*) luaL_checkudata(L, -1, "c_" #type); \
                ({ __VA_ARGS__ });                                      \
                lua_pop(L, 1);                                          \
            }                                                           \
        })
    
    #define LUA_CAST(L, idx, type) ((type*) luaL_checkudata(L, idx, "c_" #type))
}

#define APP_LUA_MOD APP_LUA_PATH "/" APP_NAME

@(extern) lua_State* default_lua_state;
@(extern) lua_CFunction* registered_c_functions;

@ bool luaA_loadfile(lua_State* L, const char* path) {
    switch (luaL_loadfile(L, path)) {
        case 0: break;
        case LUA_ERRSYNTAX:
        case LUA_ERRMEM:
        case LUA_ERRFILE:
        default: {
            const char* ret = lua_tostring(L, -1);
            fprintf(stderr, "unexpected error loading '%s': %s\n", path, ret);
            return false;
        }
    }
    return true;
}

@ bool luaA_calltop(lua_State* L) {
    switch (lua_pcall(L, 0, 0, 0)) {
        case 0: break;
        case LUA_ERRRUN:
        case LUA_ERRMEM:
        default: {
            const char* ret = lua_tostring(L, -1);
            fprintf(stderr, "unexpected error running chunk: %s\n", ret);
            return false;
        }
        case LUA_ERRERR: {
            fprintf(stderr, "error running non-existent error handler function (?)\n");
            return false;
        }
    }
    return true;
}

@ void luaA_requireinit(void) {
    #define L default_lua_state
    static bool has_init = false;
    if (!has_init) {
        int app_state = EXIT_FAILURE;
        L = luaL_newstate();
        luaL_openlibs(L);
        #ifdef APP_DEBUG
        lua_pushboolean(L, true);
        lua_setfield(L, LUA_GLOBALSINDEX, "__DEBUG");
        #endif
        lua_pushstring(L, APP_LUA_PATH);
        lua_setfield(L, LUA_GLOBALSINDEX, "__LUA_PATH");
        if (!luaA_loadfile(L, APP_LUA_MOD "/index.lua"))
            goto close;
        if (!luaA_calltop(L))
            goto close;
        lua_getfield(L, LUA_GLOBALSINDEX, "__BUILTIN_INDEX");
        lua_getfield(L, -1, "init.lua");
        {
            const char* ret = lua_tostring(L, -1);
            size_t blen = strlen(APP_LUA_MOD) + strlen(ret) + 2;
            char buf[blen];
            snprintf(buf, blen, "%s/%s", APP_LUA_MOD, ret);
            #ifdef APP_DEBUG
            printf("executing: '%s'\n", buf);
            #endif
            if (!luaA_loadfile(L, buf))
                goto close;
            if (!luaA_calltop(L))
                goto close;
        }
        lua_settop(L, 0);
        app_state = EXIT_SUCCESS;
    close:
        fflush(stdout);
        if (app_state == EXIT_FAILURE) {
            fprintf(stderr, "exiting due to fatal error\n");
            exit(app_state);
        }
        has_init = true;
    }
    #undef L
}

@ void luaA_entry(void) {
    #define L default_lua_state
    lua_getglobal(L, "entry");
    if (!lua_isfunction(L, -1)) {
        fprintf(stderr, "FATAL: Missing `entry` lua function\n");
        exit(EXIT_FAILURE);
    }
    switch (lua_pcall(L, 0, 0, 0)) {
        case 0: break;
        case LUA_ERRRUN:
        case LUA_ERRMEM:
        default: {
            const char* ret = lua_tostring(L, -1);
            fprintf(stderr, "FATAL: unexpected error running entry: %s\n", ret);
            exit(EXIT_FAILURE);
        }
        case LUA_ERRERR: {
            fprintf(stderr, "FATAL: error running non-existent error handler function (?)\n");
            exit(EXIT_FAILURE);
        }
    }
    lua_settop(L, 0);
    #undef L
}

/*
  Helper function for registering `lua_CFunction` at runtime. `loc` syntax:
  
  "foo" = _G.foo = function
  "@foo" = [registry].foo = function
  "foo.bar" = _G.foo.bar = function
  "@foo.bar" = [registry].foo.bar = function
  "@foo.! = [registry].foo.[metatable].__call = function"
  "@!" = invalid, `!` must be used on a table

  tables are created if they are missing or on type mismatch
*/
@ void luaA_register(lua_CFunction function, const char* const_loc) {
    #define L default_lua_state
    luaA_requireinit();
    int idx = LUA_GLOBALSINDEX;
    /* registry syntax */
    if (const_loc[0] == '@') {
        idx = LUA_REGISTRYINDEX;
        ++const_loc;
    }
    size_t sz = strlen(const_loc), t;
    char loc[sz + 1];
    memcpy(loc, const_loc, sz + 1);
    const char* field = NULL;
    for (t = 0; t < sz; ++t) {
        if (loc[t] == '.') {
            loc[t] = '\0';
            if (sz > t + 1)
                field = loc + t + 1;
        }
    }
    if (field != NULL) {
        lua_pushstring(L, loc);
        lua_rawget(L, idx);
        /* If the value isn't a table, pop it, create a new table, push an extra reference, and
           then set the global/registry with the new empty table, leaving us with one reference */
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            lua_newtable(L);
            /* set `__index` */
            lua_pushstring(L, "__index");
            lua_pushvalue(L, -2); /* copy of new table */
            lua_rawset(L, -3); /* `table.__index = table` */
            lua_pushstring(L, loc); /* push table name */
            lua_pushvalue(L, -2); /* copy of table, again */
            lua_rawset(L, idx); /* assign to global/registry/specified table */
        }
        idx = -3; /* Index the table on the stack instead of the global/registry table */
    }
    
    /* create metatable if `!` syntax used */
    bool mt = false;
    if (field != NULL && field[0] == '!') {
        lua_newtable(L);
        field = "__call";
        mt = true;
    }
    
    lua_pushstring(L, (const char*) (field != NULL ? field : loc));
    lua_pushcfunction(L, function);
    lua_rawset(L, idx);
    
    /* assign metatable if `!` syntax used */
    if (mt)
        lua_setmetatable(L, -2);
    #undef L
}

/*
  Helper function for constructing new userdata types with a single table argument.
  
  Automatically calls setter functions if they exist for provided entries. Assumes the
  userdata itself has already been allocated on the top of the lua stack.
*/
@ void luaA_constructor(lua_State* L, const char* type) {
    luaL_getmetatable(L, type);
    if (!lua_istable(L, -1)) {
        fprintf(stderr, "FATAL: tried to build type `%s` with no metatable!\n", type);
        exit(EXIT_FAILURE);
    }
    lua_setmetatable(L, -2);
    if (lua_gettop(L) >= 2 && lua_istable(L, 2)) {
        luaL_getmetatable(L, type);
        lua_pushnil(L); /* first key */
        while (lua_next(L, 2) != 0) {
            if (lua_isstring(L, -2)) {
                const char* k = lua_tostring(L, -2);
                size_t k_sz = strlen(k);
                char buf[k_sz + 5];
                buf[0] = 's'; buf[1] = 'e'; buf[2] = 't'; buf[3] = '_';
                memcpy(buf + 4, k, k_sz + 1);
                lua_pushstring(L, buf);
                /* stack: [args...], udata (-5), mt (-4), key (-3), value (-2), "set_[key]" (-1) */
                lua_rawget(L, -4);
                /* call setter function if it exists with value */
                if (lua_isfunction(L, -1)) {
                    lua_pushvalue(L, -5); /* udata (self) */
                    lua_pushvalue(L, -3); /* value */
                    lua_call(L, 2, 0);
                } else lua_pop(L, 1); /* pop whatever was obtained if not function */
            }
            lua_pop(L, 1); /* pop value, keep key for iteration */
        }
        lua_pop(L, 1); /* pop metatable */
    }
}

/* Expose registry to lua */
static int luaA_registry(lua_State* L) {
    lua_pushvalue(L, LUA_REGISTRYINDEX);
    return 1;
}
LUA_REGISTER_FUNCTION(luaA_registry, "registry");

/*
typedef struct {
    int a;
    char* b;
} foo;

LUA_CONSTRUCTOR(foo, {
        .a = 1,
        .b = strdup("Hello World!")
    }) {
    
}

LUA_GETTER_NUMBER(foo, a);
LUA_SETTER_NUMBER(foo, a);
LUA_GETTER_STRING(foo, b);
LUA_SETTER_STRING(foo, b);
*/

