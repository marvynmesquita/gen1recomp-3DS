-- Pokemon Yellow's Oak-speech show-off mon is the player's Pikachu, not
-- Red/Blue's NIDORINO (engine/battle/core.asm BATTLE_TYPE_PIKACHU, the
-- ProfOak demo; engine/movie/oak_speech/oak_speech.asm).  The import
-- manifest must carry field.oakSpeech.demoSpecies, and
-- Data:applyVersionedFieldData repairs Yellow caches made before the
-- manifest carried it (#915).
package.path = "./?.lua;./?/init.lua;" .. package.path
if not _G.love then _G.love = require("tests.love_stub") end

local Data = require("src.core.Data")
if not (Data.field and Data.field.oakSpeech) then Data:load() end
local GameVersion = require("src.core.GameVersion")
local S = require("tests.harness").suite("parity Yellow Oak speech")
local check, eq = S.check, S.eq

local oldVersion = GameVersion.get()
local oldTrades = Data.field.trades
local oldOldManBattle = Data.field.oldManBattle

local manifestFile = assert(io.open("tools/rom_manifest_yellow.json", "r"))
local manifest = manifestFile:read("*a")
manifestFile:close()

check(manifest:find('"demoSpecies": "PIKACHU"') ~= nil,
      "Yellow manifest stamps field.oakSpeech.demoSpecies as PIKACHU")

-- a stale Yellow cache carries shrink frames but no demoSpecies
local stale = { shrink1 = "assets/generated/intro/shrink1.png",
                shrink2 = "assets/generated/intro/shrink2.png" }
local oldOakSpeech = Data.field.oakSpeech
Data.field.oakSpeech = stale

GameVersion.set("yellow")
Data:applyVersionedFieldData()
eq(Data.field.oakSpeech.demoSpecies, "PIKACHU",
   "applyVersionedFieldData fills a stale Yellow cache with PIKACHU")

-- fill-if-absent: an importer that learns to stamp the key wins
local preStamped = { demoSpecies = "RAICHU",
                     shrink1 = "assets/generated/intro/shrink1.png" }
Data.field.oakSpeech = preStamped
Data:applyVersionedFieldData()
eq(Data.field.oakSpeech.demoSpecies, "RAICHU",
   "applyVersionedFieldData leaves an already-stamped demoSpecies alone")

Data.field.oakSpeech = oldOakSpeech
Data.field.trades = oldTrades
Data.field.oldManBattle = oldOldManBattle
GameVersion.set(oldVersion)

return S:finish()
