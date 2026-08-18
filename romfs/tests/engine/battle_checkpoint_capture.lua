-- Data-only capture of a settled battle checkpoint, including deterministic
-- gameplay RNG and normalized object-reference sets.

package.path = "./?.lua;./?/init.lua;" .. package.path
love = love or require("tests.love_stub")

local T = require("tests.harness").suite("battle checkpoint capture")
local Fixtures = require("tests.modkit").fixtures
local BattleState = require("src.battle.BattleState")
local Checkpoint = require("src.core.Checkpoint")
local Pokemon = require("src.pokemon.Pokemon")
local SaveData = require("src.core.SaveData")
local SaveSerializer = require("src.core.SaveSerializer")
local StateStack = require("src.core.StateStack")

local Data = Fixtures.fresh()
local oldGet, oldSet = love.math.getRandomState, love.math.setRandomState
local randomState = "fixture-rng-A"
love.math.getRandomState = function() return randomState end
love.math.setRandomState = function(state) randomState = state end

local function makeGame(kind)
  local save = SaveData.newGame()
  save.meta.playthroughId = "battle-playthrough"
  save.player.map, save.player.x, save.player.y = "FIX_TOWN", 2, 3
  save.party = {
    Pokemon.new(Data, "FIXMON_A", 20),
    Pokemon.new(Data, "FIXMON_C", 15),
  }
  local stack = setmetatable({ states = {} }, { __index = StateStack })
  local overworld = {
    map = { id = "FIX_TOWN" },
    player = { cellX = 2, cellY = 3, facing = "left", surfing = false },
    runner = { isRunning = function() return false end },
    parallelRunners = {}, pendingScripts = {}, parallelQueue = {}, scriptMoves = {},
  }
  function overworld:captureSave(target)
    target.player.map = self.map.id
    target.player.x, target.player.y = self.player.cellX, self.player.cellY
    target.player.facing = self.player.facing
    target.player.surfing = self.player.surfing and true or false
  end
  local game = { data = Data, save = save, stack = stack, overworld = overworld }
  stack.states[1] = overworld
  local battle
  if kind == "trainer" then
    battle = BattleState.newTrainer(game, "OPP_FIX_YOUNGSTER", 1)
    battle.checkpointOrigin = {
      kind = "trainer_encounter", map = "FIX_TOWN", npcId = "TRAINER_1",
      trainerClass = "OPP_FIX_YOUNGSTER", partyIndex = 1,
      event = "EVENT_BEAT_TRAINER_1",
    }
  else
    battle = BattleState.newWild(game, "FIXMON_B", 12)
    battle.checkpointOrigin = { kind = "wild_encounter", map = "FIX_TOWN" }
  end
  battle.phase, battle.queue = "menu", {}
  battle.onFinish = function() end
  stack.states[2] = battle
  return game, battle
end

local game, battle = makeGame("wild")
battle.turnCount = 7
battle.runAttempts = 2
battle.payDay = 45
battle.player.stages.attack = 2
battle.player.confusedTurns = 3
battle.player.curTypes = { "FIRE", "FLYING" }
local originalMoveId = battle.player.curMoves[1].id
battle.player.curMoves[1].id = "FIX_CUT"
battle.player.curMoves[1].mimic = true
battle.mimicRestores = {
  { battler = battle.player, entry = battle.player.curMoves[1], id = originalMoveId },
}
battle.enemy.mon.hp = battle.enemy.mon.hp - 4
battle.enemy.shownHP = battle.enemy.mon.hp
battle.enemy.stages.defense = -1
battle.enemy.aiLayer2 = 1
battle.enemy.thrashMove = battle.enemy.curMoves[1]
battle.enemy.thrashTurns = 2
battle.participants = { [game.save.party[1]] = true,
                        [game.save.party[2]] = true }
battle.leveledUp = { [game.save.party[2]] = true }
battle.sideToxic = { enemy = 3 }
battle.sides[1].screens.reflect = { turns = 2 }
battle.field.weather = { id = "fixture-rain", turns = 4 }

local snapshot, code, message = Checkpoint.capture(game)
T.check(snapshot ~= nil, "settled battle captures: " .. tostring(code or message))
T.eq(snapshot and snapshot.kind, "battle", "checkpoint kind is battle")
if snapshot and snapshot.kind == "battle" then
  T.eq(snapshot.rng.love, "fixture-rng-A", "LÖVE RNG state is captured")
  T.same(snapshot.runtime.overworld,
    { map = "FIX_TOWN", x = 2, y = 3, facing = "left", surfing = false },
    "return overworld point is captured")
  T.same(snapshot.runtime.battle.origin,
    { kind = "wild_encounter", map = "FIX_TOWN" },
    "semantic continuation origin is data-only")
  T.eq(snapshot.runtime.battle.turnCount, 7, "turn count is captured")
  T.eq(snapshot.runtime.battle.rulesetId, "gen1_faithful",
    "battle mechanics ruleset identity is captured")
  T.eq(snapshot.runtime.battle.runAttempts, 2, "escape attempts are captured")
  T.eq(snapshot.runtime.battle.player.stages.attack, 2,
    "player stat stages are captured")
  T.eq(snapshot.runtime.battle.player.confusedTurns, 3,
    "player volatile status is captured")
  T.same(snapshot.runtime.battle.player.curTypes, { "FIRE", "FLYING" },
    "transformed battle types are captured")
  T.same(snapshot.runtime.battle.mimicRestores,
    { { side = "player", slot = 1, id = originalMoveId } },
    "Mimic restore pointers normalize to side and move slot")
  T.eq(snapshot.runtime.battle.enemy.stages.defense, -1,
    "enemy stat stages are captured")
  T.eq(snapshot.runtime.battle.enemy.aiLayer2, 1,
    "enemy AI selection layer is captured")
  T.eq(snapshot.runtime.battle.enemy.thrashMoveSlot, 1,
    "move-instance references normalize to move slots")
  T.eq(snapshot.runtime.battle.enemy.thrashMove, nil,
    "live move-instance references are not serialized as detached copies")
  T.same(snapshot.runtime.battle.participants, { 1, 2 },
    "Pokemon-keyed participants normalize to party indices")
  T.same(snapshot.runtime.battle.leveledUp, { 2 },
    "Pokemon-keyed level-up set normalizes to party indices")
  T.same(snapshot.runtime.battle.sides[1].screens.reflect, { turns = 2 },
    "data-only side extensions are captured")
  T.same(snapshot.runtime.battle.field.weather,
    { id = "fixture-rain", turns = 4 },
    "data-only field extensions are captured")
  local encoded = SaveSerializer.encode(snapshot)
  T.check(type(encoded) == "string" and #encoded > 0,
    "battle checkpoint passes the canonical data-only serializer")

  snapshot.save.money = 1
  snapshot.runtime.battle.player.stages.attack = -6
  T.check(game.save.money ~= 1, "checkpoint progress is detached")
  T.eq(battle.player.stages.attack, 2, "checkpoint battle state is detached")
end

local trainerGame, trainer = makeGame("trainer")
trainer.enemyIndex = 1
trainer.aiUses = 2
local trainerSnapshot = Checkpoint.capture(trainerGame)
T.eq(trainerSnapshot and trainerSnapshot.kind, "battle",
  "ordinary trainer battle captures")
if trainerSnapshot and trainerSnapshot.kind == "battle" then
  T.eq(trainerSnapshot.runtime.battle.oppClass, "OPP_FIX_YOUNGSTER",
    "trainer class is captured")
  T.eq(trainerSnapshot.runtime.battle.partyIndex, 1,
    "trainer roster index is captured")
  T.eq(#trainerSnapshot.runtime.battle.enemyParty, #trainer.enemyParty,
    "complete enemy roster is captured")
end

local extensionGame, extensionBattle = makeGame("wild")
extensionBattle.field.tokens[1] = { id = "callback-token", onExpire = function() end }
local unsafe, unsafeCode = Checkpoint.capture(extensionGame)
T.check(unsafe == nil and unsafeCode == "battle_extension_unsafe",
  "callback-bearing battle extensions are rejected, not stripped")

love.math.getRandomState = nil
local rngGame = makeGame("wild")
local noRng, rngCode = Checkpoint.capture(rngGame)
T.check(noRng == nil and rngCode == "rng_state_unavailable",
  "battle capture fails closed without serializable gameplay RNG")

love.math.getRandomState, love.math.setRandomState = oldGet, oldSet
T.finish()
