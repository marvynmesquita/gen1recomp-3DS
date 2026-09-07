-- Headless integration test: load the real versaovermelha mod through the
-- mod Loader with a filesystem built from the mod's on-disk files, and
-- assert the expected registries get records.
--
-- Run from romfs/:
--   luajit tests/versaovermelha_mod_test.lua
--
-- This proves the loader PLUS the mod's entry chunk work together without
-- the live game; the 3DS runs the same romfs/ tree, so a pass here means
-- the mod will at least load on-device once the directory-probe fix lands.
package.path = "./?.lua;./?/init.lua;" .. package.path

local Loader = require("src.mods.Loader")
local T = require("tests.harness")
local check = T.check

-- ---- build a memfs from the real mod directory ------------------------
local MOD_DIR = "mods/versaovermelha"
local function buildMemFsFromDisk(modDir)
  local files = {}
  local p = io.popen('find "' .. modDir .. '" -type f')
  if p then
    for line in p:lines() do
      local rel = line:sub(#modDir + 2)
      local full = io.open(line, "rb")
      if full then
        files["mods/versaovermelha/" .. rel] = full:read("*a")
        full:close()
      end
    end
    p:close()
  end
  return files
end

local function memfs(files)
  return {
    read = function(path) return files[path] end,
    getInfo = function(path)
      if files[path] then return { type = "file", size = #files[path] } end
      local prefix = path .. "/"
      for key in pairs(files) do
        if key:sub(1, #prefix) == prefix then return { type = "directory" } end
      end
      return nil
    end,
    load = function(path)
      if not files[path] then return nil, "no file" end
      return load(files[path], path)
    end,
    getDirectoryItems = function(path)
      local seen, items = {}, {}
      local prefix = path .. "/"
      for key in pairs(files) do
        if key:sub(1, #prefix) == prefix then
          local child = key:sub(#prefix + 1):match("^[^/]+")
          if child and not seen[child] then
            seen[child] = true
            items[#items + 1] = child
          end
        end
      end
      return items
    end,
  }
end

love = require("tests.love_stub")
local files = buildMemFsFromDisk("mods/versaovermelha")
if not files["mods/versaovermelha/manifest.json"] then
  -- Third-party mod not vendored: clone https://github.com/bryanthaboi/versaovermelha
  -- into romfs/mods/versaovermelha to run this integration suite locally.
  print("SKIP versaovermelha integration: mod not present (clone it into mods/)")
  os.exit(0)
end

local loader = Loader.new({ fs = memfs(files), dev = true })
loader:load({ strings = {} })

check(#loader.errors == 0,
  "no load errors (got %d: %s)", #loader.errors, table.concat(loader.errors, "; "))

local mod = loader.mods.versaovermelha
check(mod ~= nil and mod.enabled and not mod.failed,
  "versaovermelha discovered and enabled")

-- string overrides: look for a clean PT-BR hit
local sr = loader.content.strings
local sawPT = false
if sr and sr.ops then
  for id, op in pairs(sr.ops) do
    if type(op) == "table" and op[1] and type(op[1]) == "table"
        and type(op[1].value) == "string"
        and op[1].value:find("[ÃÕÁÉÍÓÚÇãõáéíóúç]") then
      sawPT = true
      break
    end
  end
end
check(sawPT, "strings registry has at least one accented PT-BR override")

-- dialogue text override spot check
local tx = loader.content.text
local sawDialogue = false
if tx and tx.ops then
  for id, op in pairs(tx.ops) do
    if type(op) == "table" and op[1] and type(op[1]) == "table"
        and type(op[1].value) == "string"
        and op[1].value:find("[ÃÕÁÉÍÓÚÇãõáéíóúç]") then
      sawDialogue = true
      break
    end
  end
end
check(sawDialogue, "text (dialogue) registry has at least one PT-BR override")

T.finish()