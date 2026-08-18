-- Public mod.storage contract: data-only transactions, namespace isolation,
-- deterministic listing, recovery, and failure retention.

package.path = "./?.lua;./?/init.lua;" .. package.path
love = love or require("tests.love_stub")

local T = require("tests.harness").suite("mod storage")
local Loader = require("src.mods.Loader")
local Runtime = require("src.mods.Runtime")
local Version = require("src.core.Version")

local savedEvents, savedHooks = Runtime.events, Runtime.hooks

local function manifest(id)
  return ('{"id":"%s","name":"%s","version":"1.0.0",')
    :format(id, id) .. '"entry":"main.lua","api":2,"profile":"content"}'
end

local function memfs(files)
  local fs = { files = files, failTmp = false, failMain = false }

  function fs.read(path) return files[path] end
  function fs.write(path, body)
    if fs.failTmp and path:sub(-4) == ".tmp" then return false, "tmp denied" end
    if fs.failMain and path:sub(-4) == ".lua" then return false, "main denied" end
    files[path] = body
    return true
  end
  function fs.remove(path) files[path] = nil return true end
  function fs.createDirectory() return true end
  function fs.getInfo(path)
    if files[path] then return { type = "file" } end
    local prefix = path .. "/"
    for key in pairs(files) do
      if key:sub(1, #prefix) == prefix then return { type = "directory" } end
    end
    return nil
  end
  function fs.load(path)
    if not files[path] then return nil, "no file: " .. path end
    return load(files[path], path)
  end
  function fs.getDirectoryItems(path)
    local prefix, seen, out = path .. "/", {}, {}
    for key in pairs(files) do
      if key:sub(1, #prefix) == prefix then
        local child = key:sub(#prefix + 1):match("^[^/]+")
        if child and not seen[child] then
          seen[child] = true
          out[#out + 1] = child
        end
      end
    end
    table.sort(out)
    return out
  end
  return fs
end

local function game(version, playthroughId)
  return { save = {
    version = version,
    meta = { format = 4, mods = {}, playthroughId = playthroughId },
  } }
end

local files = {
  ["mods/alpha/manifest.json"] = manifest("alpha"),
  ["mods/alpha/main.lua"] = [[
return function(mod) _G.MOD_STORAGE_ALPHA = mod.storage end
]],
  ["mods/beta/manifest.json"] = manifest("beta"),
  ["mods/beta/main.lua"] = [[
return function(mod) _G.MOD_STORAGE_BETA = mod.storage end
]],
}
local fs = memfs(files)
local loader = Loader.new({ fs = fs })
local current = game("red", "play-a")
loader.game = current
T.check(loader:load({}) == true, "storage fixture mods load")

local alpha, beta = _G.MOD_STORAGE_ALPHA, _G.MOD_STORAGE_BETA
T.check(type(alpha) == "table" and type(beta) == "table",
  "Loader exposes mod.storage through the public mod object")
if type(alpha) ~= "table" or type(beta) ~= "table" then
  Runtime.events, Runtime.hooks = savedEvents, savedHooks
  _G.MOD_STORAGE_ALPHA, _G.MOD_STORAGE_BETA = nil, nil
  T.finish()
end

-- Removing scope identity or exposing a mutable private slot id breaks this.
local context = alpha:context(current)
T.same(context, {
  engineVersion = Version.engine,
  gameVersion = "red",
  playthroughId = "play-a",
}, "context exposes stable engine/game/playthrough compatibility identity")

-- Data-only write/read. The literal expected table is independent of storage.
local payload = { format = 1, nested = { money = 1234 }, flags = { a = true } }
local ok, code, message = alpha:write(current, "states/quick/q1", payload)
T.check(ok == true, "data-only payload writes: " .. tostring(code or message))
local loaded = alpha:read(current, "states/quick/q1")
T.same(loaded, payload, "stored payload roundtrips as data")
T.check(loaded ~= payload and loaded.nested ~= payload.nested,
  "read returns decoded data rather than the caller's live table")

local bad, badCode = alpha:write(current, "states/bad", { callback = function() end })
T.check(not bad and badCode == "encode_failed",
  "functions are rejected with a stable data-only error")

local escaped, escapedCode = alpha:write(current, "../escape", {})
T.check(not escaped and escapedCode == "invalid_key",
  "path traversal is rejected before persistence")

-- Logical enumeration is deterministic and prefix-scoped.
T.check(alpha:write(current, "states/quick/zeta", { n = 2 }), "write zeta")
T.check(alpha:write(current, "states/quick/alpha", { n = 1 }), "write alpha")
T.check(alpha:write(current, "settings", { enabled = true }), "write settings")
local keys = alpha:list(current, "states/quick")
T.same(keys, { "states/quick/alpha", "states/quick/q1", "states/quick/zeta" },
  "list returns sorted logical keys under the requested prefix")

-- Mod, playthrough, and game namespaces cannot observe each other.
local missing, missingCode = beta:read(current, "states/quick/q1")
T.check(missing == nil and missingCode == "not_found",
  "another mod cannot read the first mod's payload")
missing, missingCode = alpha:read(game("red", "play-b"), "states/quick/q1")
T.check(missing == nil and missingCode == "not_found",
  "another playthrough cannot read the payload")
missing, missingCode = alpha:read(game("blue", "play-a"), "states/quick/q1")
T.check(missing == nil and missingCode == "not_found",
  "another game version cannot read the payload")

-- Find the implementation-owned file only to inject corruption; assertions stay
-- on public read behavior, not the path shape.
local function mainFor(fragment)
  for path in pairs(files) do
    if path:find(fragment, 1, true) and path:sub(-4) == ".lua" then return path end
  end
end

local q1Main = mainFor("q1")
T.check(type(q1Main) == "string", "failure fixture locates the persisted q1")
files[q1Main] = "not a serialized table"
loaded, code = alpha:read(current, "states/quick/q1")
T.same(loaded, payload, "corrupt main recovers the last verified payload")
T.eq(code, nil, "successful recovery is a normal read")

-- A failed replacement cannot destroy the prior verified value.
T.check(alpha:write(current, "replace", { version = 1 }), "seed replace value")
fs.failTmp = true
ok, code = alpha:write(current, "replace", { version = 2 })
fs.failTmp = false
T.check(not ok and code == "write_failed", "staging failure is reported")
T.same(alpha:read(current, "replace"), { version = 1 },
  "staging failure leaves the prior value readable")

-- Delete is exact and idempotent-not-found is explicit.
T.check(alpha:write(current, "delete/me", { yes = true }), "seed delete target")
T.check(alpha:write(current, "delete/keep", { yes = true }), "seed delete neighbor")
T.check(alpha:delete(current, "delete/me") == true, "delete removes its target")
missing, missingCode = alpha:read(current, "delete/me")
T.check(missing == nil and missingCode == "not_found", "deleted key is unavailable")
T.same(alpha:read(current, "delete/keep"), { yes = true },
  "delete leaves neighboring keys untouched")

-- No-mod parity: constructing/loading an empty loader creates no storage bytes.
local emptyFiles, emptyFs = {}, nil
emptyFs = memfs(emptyFiles)
local emptyLoader = Loader.new({ fs = emptyFs })
emptyLoader.game = current
T.check(emptyLoader:load({}) == true, "no-mod loader still boots")
T.eq(next(emptyFiles), nil, "no-mod boot creates no storage paths or files")

Runtime.events, Runtime.hooks = savedEvents, savedHooks
Runtime.currentMod = nil
_G.MOD_STORAGE_ALPHA, _G.MOD_STORAGE_BETA = nil, nil

T.finish()
