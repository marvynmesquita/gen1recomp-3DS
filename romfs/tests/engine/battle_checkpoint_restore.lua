-- A battle checkpoint reconstructs a new controller from data, rather than
-- retaining the original table/closure, and restores gameplay RNG exactly.

package.path = "./?.lua;./?/init.lua;" .. package.path
love = love or require("tests.love_stub")

local T = require("tests.harness").suite("battle checkpoint restore")
local Fixtures = require("tests.modkit").fixtures
local BattleState = require("src.battle.BattleState")
local Checkpoint = require("src.core.Checkpoint")
local Damage = require("src.battle.Damage")
local Encounter = require("src.world.Encounter")
local GameMethods = require("src.core.Game")
local Music = require("src.core.Music")
local Pokemon = require("src.pokemon.Pokemon")
local SaveData = require("src.core.SaveData")
local SaveSerializer = require("src.core.SaveSerializer")
local StateStack = require("src.core.StateStack")
local TrainerAI = require("src.battle.TrainerAI")

local Data = Fixtures.fresh()
local oldRandom = love.math.random
local oldGet, oldSet = love.math.getRandomState, love.math.setRandomState
local oldPlayBattle = Music.playBattle
Music.playBattle = function() end
local rng = 12345
love.math.getRandomState = function() return tostring(rng) end
love.math.setRandomState = function(state) rng = assert(tonumber(state)) end
love.math.random = function(a, b)
  rng = (rng * 1103515245 + 12345) % 2147483648
  local unit = rng / 2147483648
  if a == nil then return unit end
  if b == nil then return math.floor(unit * a) + 1 end
  return a + math.floor(unit * (b - a + 1))
end

local function makeGame(kind)
  local save = SaveData.newGame()
  save.meta.playthroughId = "battle-playthrough"
  save.player.map, save.player.x, save.player.y = "FIX_TOWN", 2, 3
  save.player.facing, save.player.surfing = "left", false
  save.party = {
    Pokemon.new(Data, "FIXMON_A", 20),
    Pokemon.new(Data, "FIXMON_C", 15),
  }
  -- Strip new-game defaults that are intentionally absent from the tiny
  -- fixture registry, then place the sanitized save on a fixture map.
  SaveData.validate(save, Data)
  save.player.map, save.player.x, save.player.y = "FIX_TOWN", 2, 3
  save.player.facing, save.player.surfing = "left", false
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
  function overworld:restoreBattleContinuation(battle, origin)
    battle.onFinish = function(result)
      self.lastRestoredFinish = { result = result, origin = origin.kind }
    end
    return true
  end
  local game = setmetatable(
    { data = Data, save = save, stack = stack, overworld = overworld },
    { __index = GameMethods })
  function game:restoreCheckpointSave(loaded)
    self.save = loaded
    self.overworld.map = { id = loaded.player.map }
    self.overworld.player = {
      cellX = loaded.player.x, cellY = loaded.player.y,
      facing = loaded.player.facing,
      surfing = loaded.player.surfing and true or false,
    }
    self.overworld.runner = { isRunning = function() return false end }
    self.overworld.parallelRunners, self.overworld.pendingScripts = {}, {}
    self.overworld.parallelQueue, self.overworld.scriptMoves = {}, {}
    self.stack.states = { self.overworld }
  end
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
  battle.musicKind = battle:computeMusicKind()
  battle.onFinish = function() end
  stack.states[2] = battle
  return game, battle
end

local function settleOverworld(game)
  game.stack.states = { game.overworld }
  game.save.money = 999999
  game.save.party[1].hp = 1
  game.overworld.player.cellX = 8
  game.overworld.player.facing = "up"
end

local game, originalBattle = makeGame("wild")
originalBattle.turnCount = 4
originalBattle.runAttempts = 1
originalBattle.player.stages.speed = 3
local originalMoveId = originalBattle.player.curMoves[1].id
originalBattle.player.curMoves[1].id = "FIX_CUT"
originalBattle.player.curMoves[1].mimic = true
originalBattle.mimicRestores = {
  { battler = originalBattle.player, entry = originalBattle.player.curMoves[1],
    id = originalMoveId },
}
originalBattle.enemy.mon.hp = originalBattle.enemy.mon.hp - 5
originalBattle.enemy.shownHP = originalBattle.enemy.mon.hp
originalBattle.enemy.disabledSlot = 1
originalBattle.enemy.disabledTurns = 2
originalBattle.enemy.aiLayer2 = 1
originalBattle.enemy.thrashTurns = 2
originalBattle.enemy.mon.moves = {
  { id = "FIX_SCRATCH", pp = 35 }, { id = "FIX_CUT", pp = 30 },
}
originalBattle.enemy.curMoves = originalBattle.enemy.mon.moves
originalBattle.enemy.thrashMove = originalBattle.enemy.curMoves[1]
originalBattle.participants = { [game.save.party[1]] = true }
local checkpoint = assert(Checkpoint.capture(game))
local encounterDef = { grass = {
  rate = 256, buckets = { 128, 256 },
  slots = { { species = "FIXMON_A", level = 4 },
            { species = "FIXMON_B", level = 7 } },
} }
Encounter.load(Data)
local function randomOutcomes(battle)
  local damage, detail = Damage.compute(battle.ruleset, battle.player,
    battle.enemy, Data.moves.FIX_CUT, { rng = battle.rng })
  local hit = Damage.accuracyRoll(battle.ruleset, Data.moves.FIX_CUT,
    battle.player, battle.enemy, battle.rng)
  local ai = TrainerAI.chooseMove(battle.enemy, battle.rng, nil)
  local escaped = battle:runRollVanilla(1, 100)
  local encounter = Encounter.roll(encounterDef, love.math.random)
  local nextValue = love.math.random(1, 1000000)
  return {
    damage = damage, critical = detail.crit, hit = hit,
    ai = ai.id, escaped = escaped, encounter = encounter,
    nextValue = nextValue,
  }
end
local expectedOutcomes = randomOutcomes(originalBattle)

settleOverworld(game)
game.save.options.ruleset = "modern_clean"
rng = 777
local restored, code, message = Checkpoint.restore(game, checkpoint)
T.check(restored == true, "battle checkpoint restores: " .. tostring(message or code))
local rebuilt = restored and game.stack:top()
if restored then
  T.check(rebuilt ~= originalBattle, "restore creates a new battle controller")
  T.eq(getmetatable(rebuilt), BattleState, "restored stack top is a BattleState")
  T.eq(rebuilt.phase, "menu", "restored battle resumes at the decision menu")
  T.eq(#rebuilt.queue, 0, "restored battle has no stale action queue")
  T.eq(rebuilt.turnCount, 4, "turn count roundtrips")
  T.eq(rebuilt.runAttempts, 1, "escape state roundtrips")
  T.eq(rebuilt.player.stages.speed, 3, "player stages roundtrip")
  T.check(rebuilt.mimicRestores and rebuilt.mimicRestores[1]
      and rebuilt.mimicRestores[1].battler == rebuilt.player
      and rebuilt.mimicRestores[1].entry == rebuilt.player.curMoves[1],
    "Mimic restore references are rebuilt against the new battler")
  rebuilt:restoreMimicked(rebuilt.player)
  T.eq(rebuilt.player.curMoves[1].id, originalMoveId,
    "restored Mimic move returns to its canonical id when battle copy leaves")
  T.eq(rebuilt.player.curMoves[1].mimic, nil,
    "restored Mimic marker clears with the battle copy")
  -- Put the checkpointed battle state back before differential recapture.
  rebuilt.player.curMoves[1].id = "FIX_CUT"
  rebuilt.player.curMoves[1].mimic = true
  rebuilt.mimicRestores = {
    { battler = rebuilt.player, entry = rebuilt.player.curMoves[1], id = originalMoveId },
  }
  T.eq(rebuilt.enemy.disabledSlot, 1, "enemy volatile state roundtrips")
  T.eq(rebuilt.enemy.disabledTurns, 2, "enemy volatile duration roundtrips")
  T.eq(rebuilt.enemy.aiLayer2, 1, "enemy AI selection layer roundtrips")
  T.check(rebuilt.enemy.thrashMove == rebuilt.enemy.curMoves[1],
    "multi-turn move references rebuild against the new move list")
  T.eq(rebuilt.enemy.mon.hp, checkpoint.runtime.battle.enemyMon.hp,
    "enemy Pokemon model roundtrips")
  T.eq(game.save.money, checkpoint.save.money, "persistent progress roundtrips")
  T.eq(game.save.party[1].hp, checkpoint.save.party[1].hp,
    "party model roundtrips")
  T.eq(game.save.options.ruleset, "modern_clean",
    "current global ruleset option remains untouched")
  T.check(rebuilt.ruleset == require("src.battle.rulesets.gen1_faithful"),
    "restored battle keeps the mechanics ruleset it was captured with")
  T.same(Checkpoint.capture(game), checkpoint,
    "capture A, discard, restore A, capture A2 yields normalized A == A2")
  local replayed = randomOutcomes(rebuilt)
  T.same(replayed, expectedOutcomes,
    "damage, critical, accuracy, AI, escape, encounter and next RNG replay exactly")
  rebuilt.onFinish("run")
  T.same(game.overworld.lastRestoredFinish,
    { result = "run", origin = "wild_encounter" },
    "restored battle receives a reconstructed semantic continuation")
end

local partyGame, switchedOriginal = makeGame("wild")
partyGame.save.party[1].hp = 0
local activeMon = partyGame.save.party[2]
activeMon.status = "PAR"
activeMon.moves[1].pp = activeMon.moves[1].pp - 4
switchedOriginal.player = BattleState.makeBattler(
  Data, activeMon, true, partyGame.save)
switchedOriginal.sides[1].battlers = { switchedOriginal.player }
switchedOriginal.participants = {
  [partyGame.save.party[1]] = true,
  [partyGame.save.party[2]] = true,
}
local partyCheckpoint = assert(Checkpoint.capture(partyGame))
settleOverworld(partyGame)
partyGame.save.party[2].status = nil
partyGame.save.party[2].moves[1].pp = 1
restored, code, message = Checkpoint.restore(partyGame, partyCheckpoint)
T.check(restored == true,
  "switched/status/PP checkpoint restores: " .. tostring(message or code))
local partyRebuilt = partyGame.stack:top()
if restored then
  T.eq(partyGame.save.party[1].hp, 0,
    "fainted non-active party member roundtrips")
  T.check(partyRebuilt.player.mon == partyGame.save.party[2],
    "switched active Pokemon reconstructs against restored party identity")
  T.eq(partyRebuilt.player.mon.status, "PAR", "active status roundtrips")
  T.eq(partyRebuilt.player.mon.moves[1].pp,
    partyCheckpoint.save.party[2].moves[1].pp, "reduced PP roundtrips")
  T.check(partyRebuilt.participants[partyGame.save.party[1]] == true
      and partyRebuilt.participants[partyGame.save.party[2]] == true,
    "participant references rebuild against fainted and active party members")
  T.same(Checkpoint.capture(partyGame), partyCheckpoint,
    "switch, faint, status and PP differential recapture is exact")
end

local trainerGame, trainerOriginal = makeGame("trainer")
trainerOriginal.turnCount = 6
trainerOriginal.enemy.mon.hp = trainerOriginal.enemy.mon.hp - 3
trainerOriginal.enemy.shownHP = trainerOriginal.enemy.mon.hp
trainerOriginal.aiUses = 1
trainerOriginal.participants = { [trainerGame.save.party[1]] = true,
                                 [trainerGame.save.party[2]] = true }
local trainerCheckpoint = assert(Checkpoint.capture(trainerGame))
settleOverworld(trainerGame)
restored, code, message = Checkpoint.restore(trainerGame, trainerCheckpoint)
T.check(restored == true, "trainer checkpoint restores: " .. tostring(message or code))
local trainerRebuilt = trainerGame.stack:top()
if restored then
  T.check(trainerRebuilt ~= trainerOriginal,
    "trainer restore is independent of the original controller")
  T.eq(trainerRebuilt.oppClass, "OPP_FIX_YOUNGSTER", "trainer class roundtrips")
  T.eq(trainerRebuilt.enemyIndex, 1, "enemy roster index roundtrips")
  T.eq(trainerRebuilt.aiUses, 1, "trainer AI item budget roundtrips")
  T.same(Checkpoint.capture(trainerGame), trainerCheckpoint,
    "trainer differential recapture is exact")
end

local function clone(value)
  return assert(SaveSerializer.decode(SaveSerializer.encode(value)))
end

local beforeRejected = assert(Checkpoint.capture(trainerGame))
local missingSpecies = clone(trainerCheckpoint)
missingSpecies.runtime.battle.enemyParty[1].species = "MISSING_SPECIES"
restored, code = Checkpoint.restore(trainerGame, missingSpecies)
T.check(restored == false and code == "invalid_content",
  "unknown battle content is rejected before mutation")
T.same(Checkpoint.capture(trainerGame), beforeRejected,
  "rejected battle content leaves runtime and RNG unchanged")

local badOrigin = clone(trainerCheckpoint)
badOrigin.runtime.battle.origin.npcId = nil
restored, code = Checkpoint.restore(trainerGame, badOrigin)
T.check(restored == false and code == "battle_origin_unsupported",
  "incomplete semantic continuation is rejected before mutation")
T.same(Checkpoint.capture(trainerGame), beforeRejected,
  "rejected continuation leaves runtime and RNG unchanged")

-- Fail after the new battle has been installed, when its RNG is applied.
-- The transaction must reconstruct the prior battle and restore its RNG.
local workingSetRandomState = love.math.setRandomState
local setCalls = 0
love.math.setRandomState = function(state)
  setCalls = setCalls + 1
  if setCalls == 1 then error("injected RNG restore failure") end
  return workingSetRandomState(state)
end
local beforeFailure = assert(Checkpoint.capture(trainerGame))
local rngBeforeFailure = rng
restored, code = Checkpoint.restore(trainerGame, trainerCheckpoint)
T.check(restored == false and code == "restore_failed",
  "post-install RNG failure is returned as a structured restore failure")
T.eq(rng, rngBeforeFailure, "failed battle restore rolls RNG back exactly")
T.same(Checkpoint.capture(trainerGame), beforeFailure,
  "failed battle restore rolls the complete runtime back exactly")
love.math.setRandomState = workingSetRandomState

love.math.random = oldRandom
love.math.getRandomState, love.math.setRandomState = oldGet, oldSet
Music.playBattle = oldPlayBattle
T.finish()
