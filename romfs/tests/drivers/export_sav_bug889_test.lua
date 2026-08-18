-- Driver (#889): export a .sav from a save that never came from a ROM import,
-- the case that used to write a save with no map context at all -- no map
-- header, no tileset header, sound id 0 -- so a real Game Boy continued into a
-- garbled map and hung on a white screen.
--
-- Saves the game in two places (an interior and an outdoor map with
-- connections), exports each, and prints the bytes the engine reads back on
-- Continue so a human can eyeball them, plus the export path to load in an
-- emulator.
return function(game)
  local U = dofile("tests/drivers/util.lua")
  local SaveFileIO = require("src.import.SaveFileIO")
  local SaveData = require("src.core.SaveData")
  local GameVersion = require("src.core.GameVersion")

  local version = GameVersion.get()
  local spots = {
    { "REDS_HOUSE_2F", 3, 6 },
    { "PALLET_TOWN", 5, 6 },
  }

  for _, spot in ipairs(spots) do
    local map, x, y = spot[1], spot[2], spot[3]
    U.teleport(game, map, x, y, "down")
    U.wait(30)
    -- the same sync the in-game SAVE does before writing (OverworldState:
    -- captureSave), so the slot on disk holds the position we just walked to
    local top = game.stack:top()
    if top and top.captureSave then top:captureSave(game.save) end
    SaveData.save(game.save)
    local ok, pathOrErr = SaveFileIO.exportActiveSlot(version)
    if not ok then
      U.log(("export_sav_bug889: %s FAILED: %s"):format(map, tostring(pathOrErr)))
    else
      local bytes = love.filesystem.read(
        ("exports/%s/gen1recomp-%s-%s.sav"):format(
          version, version, SaveData.activeSlot(version) or "save"))
      local main = 0x2598 + 11
      local function u8(off) return bytes:byte(main + off + 1) end
      local hdr = {}
      for i = 0, 9 do hdr[#hdr + 1] = ("%02X"):format(u8(112 + i)) end
      U.log(("export_sav_bug889: %s -> %s"):format(map, pathOrErr))
      U.log(("  wCurMap=%02X wCurMapHeader=%s music=%02X/%02X tilesetBank=%02X"):
        format(u8(103), table.concat(hdr, " "), u8(100), u8(101), u8(564)))
    end
  end

  U.log("export_sav_bug889: done")
  love.event.quit()
  while true do coroutine.yield() end
end
