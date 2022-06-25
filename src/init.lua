
function entry()
  C = {}
  for k, v in pairs(registry()) do
    if k:sub(1, 2) == "c_" then
      k = k:sub(3)
      C[k] = v
    end
  end
  io.file_remove = function(...) C.async_file_remove(...) end
  io.dir_remove  = function(...) C.async_dir_remove (...) end
  io.dir_create  = function(...) C.async_dir_create (...) end
  io.open  = C.async_fd_open
  io.read  = function(self, ...) self:read (...) end
  io.write = function(self, ...) self:write(...) end
  io.flush = function(self, ...) self:flush(...) end
  io.close = function(self, ...) self:close(...) end
  io.lines = function(filename)
    if filename == nil then
      error("`io.lines` cannot be used on stdin")
    end
    local file = io.open(filename, "r")
    return function()
      local line = file:read("*l")
      if line == nil then file:close() end
      return line
    end
  end
  function C.async_fd:lines()
    return function()
      return self:read("*l")
    end
  end
  local function unsupported(...)
    for _, v in ipairs({...}) do
      local t = split(v, "%.")
      local tbl = _G
      for k, s in ipairs(t) do
        if k == #t then
          tbl[s] = function()
            error("`" .. f .. "` is unsupported in this enviornment (non-portable function)")
          end
        else
          tbl = tbl[s]
        end
      end
    end
  end
  unsupported("io.popen", "io.input", "io.output", "io.tmpfile")
  
  -- NOTE: direct buffers are used; no flushing supported
  function C.async_fd:flush() end
  function C.async_fd:setvbuf(mode, size) end
  if __DEBUG then
    print("__LUA_PATH = \"" .. __LUA_PATH .. "\"")
    print("package.path = \"" .. package.path .. "\"")
    print("")
  end
end

function split(str, pat)
   local t = {}  -- NOTE: use {n = 0} in Lua-5.0
   local fpat = "(.-)" .. pat
   local last_end = 1
   local s, e, cap = str:find(fpat, 1)
   while s do
      if s ~= 1 or cap ~= "" then
         table.insert(t, cap)
      end
      last_end = e+1
      s, e, cap = str:find(fpat, last_end)
   end
   if last_end <= #str then
      cap = str:sub(last_end)
      table.insert(t, cap)
   end
   return t
end

function remove_empty(list)
  local collected = {}
  for i = 1, #list do
    local v = list[i]
    if v ~= "" then
      collected[#collected + 1] = v
    end
  end
end

local __builtin_require = require
require = function(mod)
  if package.loaded[mod] == nil then
    local modp = table.concat(split(mod, "%."), "/")
    local resolved
    if __BUILTIN_INDEX[modp .. ".lua"] ~= nil then
      resolved = __BUILTIN_INDEX[modp .. ".lua"]
      return
    elseif __BUILTIN_INDEX[modp .. "/init.lua"] ~= nil then
      resolved = __BUILTIN_INDEX[modp .. "/init.lua"]
    else
      return __builtin_require(mod)
    end
    local ret = dofile(__LUA_PATH .. "/" .. resolved)
    package.loaded[mod] = ret
    return ret
  end
  return __builtin_require(mod)
end
