-- Opaque playthrough identity: New Game uniqueness, save/load persistence,
-- stable legacy backfill, and version/slot isolation. No real save directory.

package.path = "./?.lua;./?/init.lua;" .. package.path

local T = require("tests.harness")
love = love or require("tests.love_stub")

local SaveData = require("src.core.SaveData")
local SaveSerializer = require("src.core.SaveSerializer")
local GameVersion = require("src.core.GameVersion")

local realFS = love.filesystem

local function memfs(files)
  return {
    write = function(path, content) files[path] = content return true end,
    read = function(path) return files[path] end,
    remove = function(path) files[path] = nil return true end,
    createDirectory = function() return true end,
    getInfo = function(path)
      if files[path] then return { type = "file" } end
      local prefix = path .. "/"
      for key in pairs(files) do
        if key:sub(1, #prefix) == prefix then return { type = "directory" } end
      end
      return nil
    end,
    getDirectoryItems = function(path)
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
    end,
  }
end

local function fresh()
  local files = {}
  love.filesystem = memfs(files)
  SaveData.resetSlotState()
  GameVersion.set("red")
  return files
end

local function legacy(version, name)
  return {
    version = version,
    meta = { format = 4, mods = {} },
    player = { name = name, map = "PALLET_TOWN", x = 5, y = 6 },
    flags = {}, inventory = {}, pcItems = {}, party = {}, box = {}, boxes = {},
    money = 3000, defeatedTrainers = {}, pokedex = { seen = {}, owned = {} },
  }
end

-- No-mod parity: creating/saving a vanilla playthrough allocates no tool scope.
do
  fresh()
  local first = SaveData.newGame({ version = "red" })
  local second = SaveData.newGame({ version = "red" })
  T.eq(first.meta.playthroughId, nil,
    "New Game allocates no playthrough id before a public tool requests it")
  T.check(SaveData.save(first), "unused identity fixture saves")
  local untouched = SaveData.load("red")
  T.eq(untouched.meta.playthroughId, nil,
    "normal save/load stays identity-free when no tool uses the capability")

  local firstId = SaveData.ensurePlaythroughId(first)
  local secondId = SaveData.ensurePlaythroughId(second)
  T.check(type(firstId) == "string" and firstId ~= "",
    "the first tool request allocates an opaque playthrough id")
  T.neq(secondId, firstId,
    "separate New Games receive separate requested playthrough ids")
end

-- Dropping the id from buildMeta or save encoding must fail the roundtrip.
do
  fresh()
  local save = SaveData.newGame({ version = "red" })
  local expected = SaveData.ensurePlaythroughId(save)
  T.check(SaveData.save(save), "identity fixture saves")
  local loaded = SaveData.load("red")
  T.eq(loaded and loaded.meta.playthroughId, expected,
    "normal save/load preserves the playthrough id")
end

-- Legacy identity is persisted independently: the legacy progress bytes remain
-- unchanged, yet two loads resolve the same id before a normal SAVE occurs.
do
  local files = fresh()
  local raw = legacy("red", "LEGACY")
  files["save.lua"] = SaveSerializer.encode(raw)

  local first = SaveData.load("red")
  T.eq(first and first.meta.playthroughId, nil,
    "loading a legacy save alone does not allocate tool identity")
  local id = SaveData.ensurePlaythroughId(first)
  T.check(type(id) == "string" and id ~= "",
    "a legacy save receives a playthrough id")
  local mappedOptions, mappedErr = SaveSerializer.decode(files["options.lua"] or "")
  T.check(mappedOptions ~= nil,
    "legacy identity mapping remains decodable: " .. tostring(mappedErr))

  local slotBytes = files["saves/red/slot1.lua"]
  local onDisk = slotBytes and SaveSerializer.decode(slotBytes)
  T.eq(onDisk and onDisk.meta.playthroughId, nil,
    "legacy backfill does not rewrite normal progress")

  SaveData.resetSlotState()
  local second = SaveData.load("red")
  T.eq(SaveData.ensurePlaythroughId(second), id,
    "legacy backfill is stable across reload before normal SAVE")
end

-- Reusing names and coordinates cannot merge identities across slots or games.
do
  fresh()
  local redA = SaveData.createSlot("red")
  local redB = SaveData.createSlot("red")
  SaveData.setActiveSlot("red", redA)
  T.check(SaveData.writeSlot("red", redA, legacy("red", "SAME")),
    "seed red slot A")
  local idA = SaveData.ensurePlaythroughId(SaveData.load("red"))

  SaveData.setActiveSlot("red", redB)
  T.check(SaveData.writeSlot("red", redB, legacy("red", "SAME")),
    "seed red slot B")
  local idB = SaveData.ensurePlaythroughId(SaveData.load("red"))

  GameVersion.set("blue")
  local blue = SaveData.createSlot("blue")
  SaveData.setActiveSlot("blue", blue)
  T.check(SaveData.writeSlot("blue", blue, legacy("blue", "SAME")),
    "seed blue slot")
  local idBlue = SaveData.ensurePlaythroughId(SaveData.load("blue"))

  T.neq(idA, idB, "two active slots do not share legacy identity")
  T.neq(idA, idBlue, "Red and Blue do not share legacy identity")
  T.neq(idB, idBlue, "every version/slot scope is isolated")
end

love.filesystem = realFS
SaveData.resetSlotState()
GameVersion.set("red")

T.finish("playthrough_identity")
