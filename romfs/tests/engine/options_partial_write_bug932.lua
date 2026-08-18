-- #932 "Bugs reset settings": a caller that hands saveOptions a PARTIAL
-- table (only the keys it changed) used to drop every key it did not
-- mention -- launcher-only keys like lastVersion, and keys the launcher set
-- (battleBg, tilt) all fell back to defaults.  saveOptions now reads the
-- on-disk file first and folds caller-absent values underneath, so a delta
-- write changes only what it names.
--
-- This suite pins the three-way merge against injected filesystem stubs
-- (the same { getInfo, read, write, remove } shape the other engine suites
-- use).  It is ROM-free (T2 engine tier).
--   luajit tests/engine/options_partial_write_bug932.lua

package.path = "./?.lua;./?/init.lua;" .. package.path

local T = require("tests.harness")
local check, eq = T.check, T.eq
love = love or require("tests.love_stub")

local SaveData = require("src.core.SaveData")

local OPTIONS = "options.lua"

local function memfs()
  local files = {}
  return {
    files = files,
    write = function(path, content) files[path] = content return true end,
    read = function(path) return files[path] end,
    remove = function(path) files[path] = nil return true end,
    getInfo = function(path)
      if files[path] ~= nil then return { type = "file" } end
      return nil
    end,
  }
end

-- Seed a save dir with the full snapshot a launcher would write: defaults,
-- plus the keys the issue cares about.  lastVersion is launcher-only (not a
-- defaultOptions member) and must survive ANY write that does not name it.
local function seed(fs)
  local seed = SaveData.defaultOptions()
  seed.battleBg = "world"
  seed.lastVersion = "blue"
  seed.tilt = 1
  seed.mods = { foo = true }
  seed.modOptions = { alpha = { keep = true, x = 1 } }
  check(SaveData.saveOptions(seed, fs) ~= nil, "seeding lands")
end

-- ---- launcher-only keys survive a delta write

local fs = memfs()
seed(fs)

-- loader-style partial write: only the mods bucket it manages.
SaveData.saveOptions({ mods = { foo = true } }, fs)
local opts = SaveData.loadOptions(fs)
eq(opts.battleBg, "world", "a partial write keeps battleBg the launcher set")
eq(opts.lastVersion, "blue", "a partial write keeps lastVersion (#932)")
eq(opts.tilt, 1, "a partial write keeps tilt the launcher set")

-- ---- caller-present keys still win

SaveData.saveOptions({ battleBg = "black" }, fs)
eq(SaveData.loadOptions(fs).battleBg, "black",
  "a key the caller DOES provide wins over the on-disk value")
eq(SaveData.loadOptions(fs).lastVersion, "blue",
  "...while the launcher-only key is still carried")

-- ---- modOptions per-mod deep merge stays intact

SaveData.saveOptions({ modOptions = { alpha = { x = 5 } } }, fs)
local after = SaveData.loadOptions(fs)
eq(after.modOptions.alpha.x, 5, "newest alpha value wins the per-mod merge")
eq(after.modOptions.alpha.keep, true, "alpha's untouched keys survive")
eq(after.modOptions.beta, nil, "no beta was invented by the merge")

-- ---- full-table writes stay authoritative (bindings/activeProfile drops)

-- The fold must NOT resurrect a key a full snapshot deliberately deletes:
-- the RESET REBINDS path nils bindings and the mod manager nils
-- activeProfile, always on full loadOptions tables.
fs = memfs()
seed(fs)
SaveData.saveOptions({ bindings = { a = 1 } }, fs)
eq(SaveData.loadOptions(fs).bindings.a, 1, "bindings is not a default member")

local full = SaveData.loadOptions(fs)
full.bindings = nil
full.activeProfile = nil
SaveData.saveOptions(full, fs)
local reopened = SaveData.loadOptions(fs)
eq(reopened.bindings, nil,
  "a full-snapshot deletion of bindings is NOT resurrected by the fold")
eq(reopened.activeProfile, nil,
  "a full-snapshot deletion of activeProfile is NOT resurrected")
eq(reopened.battleBg, "world",
  "the rest of the full snapshot is still what it was")

T.finish("options_partial_write_bug932")
