package.path = "./?.lua;./?/init.lua;" .. package.path

local T = require("tests.harness")
local name = "src.render.SecondScreen"
local oldModule = package.loaded[name]
local oldFfi = package.loaded.ffi
local oldPreload = package.preload.ffi
local null = {}
local calls = 0
local C = {
  love_android_secondary_ready = function() return 1 end,
  love_android_push_secondary = function() end,
  love_android_secondary_enable = function() end,
  love_android_poll_secondary_touch = function()
    calls = calls + 1
    return calls == 1 and "down,12,34" or null
  end,
}
local fakeFfi = {
  C = C,
  NULL = null,
  cdef = function() end,
  load = function() return C end,
  string = function(value) return value end,
}

package.loaded[name] = nil
package.loaded.ffi = nil
package.preload.ffi = function() return fakeFfi end

local SecondScreen = require(name)
T.eq(SecondScreen.pollTouch(), "down,12,34",
  "secondary touch reaches the Lua facade")
T.eq(SecondScreen.pollTouch(), nil, "an empty native touch queue returns nil")
C.love_android_poll_secondary_touch = nil
T.eq(SecondScreen.pollTouch(), nil, "an older native bridge remains safe")

package.loaded[name] = oldModule
package.loaded.ffi = oldFfi
package.preload.ffi = oldPreload

T.finish("second-screen touch facade")
