-- Bridge to native secondary-display output (Android Presentation). The C
-- functions live in mobile/android/love/src/jni/love/src/common/android.cpp.
-- Everything is guarded: off Android, or if the symbols cannot be resolved,
-- this stays inert and the renderer keeps the in-window stacked layout.

local SecondScreen = {}
local C = nil
local ffi = nil

local function log(msg)
  pcall(function() require("src.core.Logger").info("SecondScreen: %s", msg) end)
end

do
  local ok
  ok, ffi = pcall(require, "ffi")
  if not (ok and ffi) then
    log("ffi unavailable (not LuaJIT); second display disabled")
  else
    pcall(ffi.cdef, [[
      int love_android_secondary_ready();
      void love_android_push_secondary(const void *rgba, int w, int h);
      void love_android_secondary_enable(int on);
      const char *love_android_poll_secondary_touch();
    ]])
    local okLib, lib = pcall(ffi.load, "love")
    if okLib and lib and pcall(function() return lib.love_android_secondary_ready end) then
      C = lib
      log("bridge linked via ffi.load('love')")
    elseif pcall(function() return ffi.C.love_android_secondary_ready end) then
      C = ffi.C
      log("bridge linked via default namespace")
    else
      log(("bridge symbols not found (ffi.load ok=%s); second display disabled")
        :format(tostring(okLib)))
    end
  end
end

function SecondScreen.usable()
  return C ~= nil
end

function SecondScreen.available()
  if not C then return false end
  local ok, r = pcall(C.love_android_secondary_ready)
  return ok and r ~= 0
end

function SecondScreen.push(imageData, w, h)
  if not C or not imageData then return false end
  return pcall(function()
    C.love_android_push_secondary(imageData:getFFIPointer(), w, h)
  end)
end

-- Returns the oldest queued secondary-display event as "action,x,y", where
-- coordinates are in the submitted frame's pixel space.
function SecondScreen.pollTouch()
  if not C then return nil end
  local ok, event = pcall(function()
    return C.love_android_poll_secondary_touch()
  end)
  if not ok or event == nil or event == ffi.NULL then return nil end
  return ffi.string(event)
end

function SecondScreen.setEnabled(on)
  if not C then return end
  pcall(function() C.love_android_secondary_enable(on and 1 or 0) end)
end

return SecondScreen
