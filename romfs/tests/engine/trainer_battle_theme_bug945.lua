-- Issue #945: a mod's per-trainer battleTheme (trainers.battleTheme, an
-- audio.songs id) was validated and merged onto the trainer record but
-- never read -- battle music came solely from data.audio.battle[kind] where
-- kind is computeMusicKind()'s final/gym/trainer/wild.  Both battle-theme
-- start sites (OverworldController:pushBattle's pre-wipe cue and
-- BattleState:enter) now route through BattleState:playBattleTheme(), which
-- hands the override to Music.playBattle's new song arg.  A nil override
-- keeps the kind default, so vanilla trainer fights -- and #782's non-gym
-- Giovanni -- are unchanged.
package.path = "./?.lua;./?/init.lua;" .. package.path

local T = require("tests.modkit")

-- ------- love audio stub: file-backed songs only (mod_audio pattern)

local love = _G.love or {}
_G.love = love
love.audio = love.audio or {}

local assets = {
  ["assets/theme.ogg"] = true,
  ["assets/alt.ogg"] = true,
}

local sources = {}

local Source = {}
Source.__index = Source
function Source:play() end
function Source:stop() end
function Source:setLooping() end
function Source:setVolume() end
function Source:setFilter() end

love.audio.newSource = function(what, mode)
  if type(what) == "string" and not assets[what] then
    error("could not open file " .. what, 0)
  end
  local src = setmetatable({ file = what, mode = mode, queueable = false }, Source)
  sources[#sources + 1] = src
  return src
end
love.audio.newQueueableSource = function()
  local src = setmetatable({ queueable = true }, Source)
  sources[#sources + 1] = src
  return src
end

local Music = require("src.core.Music")
local Runtime = require("src.mods.Runtime")
local Font = require("src.render.Font")
local TypeChart = require("src.battle.TypeChart")
local Pokemon = require("src.pokemon.Pokemon")
local SaveData = require("src.core.SaveData")
local BattleState = require("src.battle.BattleState")

-- ------- the fix-945 mod: register a song and point a trainer class at it

local MOD = {
  ["mods/fix_youngster_theme/manifest.json"] = [[{
    "id": "fix_youngster_theme",
    "name": "Fix Youngster Theme",
    "version": "1.0.0",
    "entry": "main.lua",
    "api": 2
  }]],
  ["mods/fix_youngster_theme/main.lua"] = [[
    local mod = ...
    mod.content.music:register("Music_ModTheme", { file = "assets/theme.ogg" })
    mod.content.trainers:patch("OPP_FIX_YOUNGSTER", {
      battleTheme = "Music_ModTheme",
    })
  ]],
}

local function newGame(data)
  local save = SaveData.newGame()
  save.player.name = "RED"
  save.player.rival = "GARY"
  save.party = { Pokemon.new(data, "FIXMON_A", 30) }
  return { data = data, save = save,
           stack = { top = function() return nil end,
                     push = function() end, pop = function() end } }
end

-- record every cue the music.select hook sees
local function hookRecorder(seen)
  Runtime.hooks:wrap("music.select", function(nextLink, song, ctx)
    seen[#seen + 1] = { song = song, kind = ctx.kind,
                        trainerId = ctx.trainerId }
    return nextLink(song, ctx)
  end, nil, "bug945")
end

-- a playBattle spy that records (kind, trainerId, song) without touching audio
local function spyPlayBattle()
  local calls = {}
  local real = Music.playBattle
  Music.playBattle = function(data, kind, trainerId, song)
    calls[#calls + 1] = { kind = kind, trainerId = trainerId, song = song }
  end
  return calls, function() Music.playBattle = real end
end

-- ------- the modded class resolves and the override reaches the cue

local Data = T.fixtures.fresh()
Font.load(Data)
TypeChart.load(Data)

local run = T.sdk.loadMods({ "mods/fix_youngster_theme" },
                           { data = Data, fs = T.sdk.memfs(MOD) })
T.eq(#run.errors, 0, "the battleTheme mod loads without validation errors")
T.eq(Data.trainers.OPP_FIX_YOUNGSTER.battleTheme, "Music_ModTheme",
     "the patch lands on the trainer record")

-- give the kind defaults a home so the no-override fallback is observable
Data.audio = Data.audio or {}
Data.audio.battle = Data.audio.battle or {
  wild = "Music_DefaultWild", trainer = "Music_DefaultTrainer",
}
Data.audio.songs = Data.audio.songs or {}
Data.audio.songs.Music_DefaultWild = { file = "assets/alt.ogg" }
Data.audio.songs.Music_DefaultTrainer = { file = "assets/alt.ogg" }

local battle = BattleState.newTrainer(newGame(Data), "OPP_FIX_YOUNGSTER", 1)
T.eq(battle:battleTheme(), "Music_ModTheme",
     "battleTheme() resolves the per-trainer override")
T.eq(battle:computeMusicKind(), "trainer",
     "a plain trainer fight is still trainer-kind")

local calls, restore = spyPlayBattle()
battle:playBattleTheme()
T.eq(#calls, 1, "playBattleTheme cues the theme once")
T.eq(calls[1].kind, "trainer", "the cue carries the computed kind")
T.eq(calls[1].trainerId, "OPP_FIX_YOUNGSTER", "the cue carries the trainer id")
T.eq(calls[1].song, "Music_ModTheme", "the override label wins over the kind default")

-- enter() sets self.musicKind before playing; playBattleTheme honors it
battle.musicKind = "gym"
battle:playBattleTheme()
T.eq(calls[2].kind, "gym", "a pre-set musicKind (the enter path) is used as-is")
restore()

-- ------- Music.playBattle: override arg wins; nil falls back to the default

local seen = {}
hookRecorder(seen)
Music.reload()
Music.playBattle(Data, "trainer", "OPP_FIX_YOUNGSTER", "Music_ModTheme")
T.eq(seen[1].song, "Music_ModTheme", "the override arg is played")
T.eq(seen[1].kind, "trainer", "the hook sees the battle kind")
T.eq(seen[1].trainerId, "OPP_FIX_YOUNGSTER", "the hook sees the trainer id")

Music.reload()
Music.playBattle(Data, "trainer", "OPP_FIX_YOUNGSTER")
T.eq(seen[2].song, "Music_DefaultTrainer",
     "no override falls back to the kind's default song")
T.eq(seen[2].trainerId, "OPP_FIX_YOUNGSTER",
     "the hook still sees the trainer id on the default path")

-- ------- a vanilla class has no override, so the kind default is untouched

local DataV = T.fixtures.fresh()
Font.load(DataV)
TypeChart.load(DataV)
DataV.audio = {
  battle = { wild = "Music_DefaultWild", trainer = "Music_DefaultTrainer" },
  songs = {
    Music_DefaultWild = { file = "assets/alt.ogg" },
    Music_DefaultTrainer = { file = "assets/alt.ogg" },
  },
}

local battleV = BattleState.newTrainer(newGame(DataV), "OPP_FIX_YOUNGSTER", 1)
T.eq(battleV:battleTheme(), nil, "a vanilla trainer class has no override")
local callsV, restoreV = spyPlayBattle()
battleV:playBattleTheme()
T.eq(callsV[1].kind, "trainer", "vanilla cue keeps the trainer kind")
T.eq(callsV[1].song, nil, "vanilla passes no override, so the default plays (#782)")
restoreV()

local seenV = {}
hookRecorder(seenV)
Music.reload()
Music.playBattle(DataV, "trainer", "OPP_FIX_YOUNGSTER")
T.eq(seenV[1].song, "Music_DefaultTrainer",
     "vanilla battles play the kind default, not a per-trainer theme (#782)")

T.finish("trainer battle theme bug945")
