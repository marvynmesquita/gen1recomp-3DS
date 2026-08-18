-- GameActivity exposes an optional Android host seam without requiring any
-- host implementation (or its native libraries) in the stock build.
-- Self-contained: luajit tests/engine/android_host_extension_test.lua
local path = "mobile/android/love/src/main/java/org/love2d/android/GameActivity.java"
local file = assert(io.open(path, "rb"))
local source = file:read("*a")
file:close()

local function check(value, message)
  if not value then error(message, 2) end
end

local function position(text)
  local p = source:find(text, 1, true)
  check(p, "missing: " .. text)
  return p
end

check(source:find("protected String[] getHostLibraries()", 1, true),
  "host libraries are an overridable protected extension")
check(source:find("return new String[0];", 1, true),
  "the vanilla host library list is empty")

local cpp = position('libraries[0] = "c++_shared";')
local mpg = position('libraries[1] = "mpg123";')
local openal = position('libraries[2] = "openal";')
local host = position("System.arraycopy(hostLibraries, 0, libraries, 3, hostLibraries.length);")
local love = position('libraries[libraries.length - 1] = "love";')
check(cpp < mpg and mpg < openal and openal < host and host < love,
  "optional libraries load after dependencies while liblove remains last")

for _, hook in ipairs({
  "onHostCreateBeforeSDL", "onHostCreateAfterSDL", "onHostResume",
  "onHostPause", "onHostDestroy",
}) do
  check(source:find("protected void " .. hook, 1, true),
    hook .. " is a protected extension hook")
end

check(position("onHostCreateBeforeSDL(savedInstanceState);") <
      position("super.onCreate(savedInstanceState);") and
      position("super.onCreate(savedInstanceState);") <
      position("onHostCreateAfterSDL(savedInstanceState);"),
  "create hooks bracket SDL creation")
check(position("super.onResume();") < position("onHostResume();"),
  "resume hook runs after SDL resumes")
check(position("onHostPause();") < position("super.onPause();"),
  "pause hook runs before SDL pauses")
check(position("onHostDestroy();") < position("super.onDestroy();"),
  "destroy hook runs before SDL destruction")

check(not source:lower():find("openxr", 1, true),
  "generic Android activity must not require OpenXR")
check(not source:find("QuestActivity", 1, true) and
      not source:find("QuestBridge", 1, true),
  "generic Android activity must not require Quest classes")

print("android_host_extension_test: ok")
