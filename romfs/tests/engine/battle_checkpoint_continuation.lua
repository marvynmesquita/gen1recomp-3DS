-- Engine-owned battle continuations replace unserializable onFinish closures
-- after a persistent checkpoint reconstructs the overworld and battle.

package.path = "./?.lua;./?/init.lua;" .. package.path
love = love or require("tests.love_stub")

local T = require("tests.harness").suite("battle checkpoint continuation")
local GameMethods = require("src.core.Game")
local OverworldState = require("src.world.OverworldController")

local function fakeOverworld()
  local npc = { id = "FIX_TOWN_obj_1", frozen = true }
  local ow = setmetatable({
    map = { id = "FIX_TOWN" },
    npcPool = { [npc.id] = npc },
    engaging = true,
  }, { __index = OverworldState })
  ow.afterBattle = function(self, result, battle)
    self.after = { result = result, battle = battle }
  end
  ow.checkVictoryRewards = function(self, class, party)
    self.reward = { class = class, party = party }
  end
  return ow, npc
end

local wildOw = fakeOverworld()
local wildGame = { save = { defeatedTrainers = {}, flags = {} } }
local wild = { game = wildGame, kind = "wild" }
T.check(wildOw:restoreBattleContinuation(wild,
  { kind = "wild_encounter", map = "FIX_TOWN" }) == true,
  "ordinary wild continuation binds")
wild.onFinish("run")
T.same(wildOw.after, { result = "run", battle = wild },
  "wild continuation returns through canonical afterBattle")

local trainerOw, trainerNpc = fakeOverworld()
local trainerGame = { save = { defeatedTrainers = {}, flags = {} } }
local trainer = {
  game = trainerGame, kind = "trainer",
  oppClass = "OPP_FIX_YOUNGSTER", partyIndex = 1,
}
local trainerOrigin = {
  kind = "trainer_encounter", map = "FIX_TOWN",
  npcId = trainerNpc.id, trainerClass = trainer.oppClass, partyIndex = 1,
  event = "EVENT_BEAT_FIX_TRAINER",
}
T.check(trainerOw:restoreBattleContinuation(trainer, trainerOrigin) == true,
  "ordinary trainer continuation binds")
trainer.onFinish("win")
T.check(trainerGame.save.defeatedTrainers[trainerNpc.id] == true,
  "trainer win stamps the stable object id")
T.check(trainerGame.save.flags.EVENT_BEAT_FIX_TRAINER == true,
  "trainer win stamps the header event")
T.same(trainerOw.reward,
  { class = "OPP_FIX_YOUNGSTER", party = 1 },
  "trainer win runs canonical victory rewards")
T.same(trainerOw.after, { result = "win", battle = trainer },
  "trainer win returns through canonical afterBattle")
T.check(trainerOw.engaging == false and trainerNpc.frozen == false,
  "reconstructed trainer completion leaves overworld input unfrozen")

local lossOw, lossNpc = fakeOverworld()
local lossGame = { save = { defeatedTrainers = {}, flags = {} } }
local lossBattle = {
  game = lossGame, kind = "trainer",
  oppClass = "OPP_FIX_YOUNGSTER", partyIndex = 1,
}
T.check(lossOw:restoreBattleContinuation(lossBattle, trainerOrigin) == true,
  "trainer loss continuation binds")
lossBattle.onFinish("lose")
T.eq(lossGame.save.defeatedTrainers[lossNpc.id], nil,
  "trainer loss does not stamp the trainer defeated")
T.eq(lossGame.save.flags.EVENT_BEAT_FIX_TRAINER, nil,
  "trainer loss does not stamp the header event")
T.eq(lossOw.reward, nil, "trainer loss does not grant victory rewards")

local mismatchOw = fakeOverworld()
T.check(mismatchOw:restoreBattleContinuation(trainer, {
  kind = "trainer_encounter", map = "OTHER_MAP", npcId = trainerNpc.id,
  trainerClass = trainer.oppClass, partyIndex = 1,
}) == false, "continuation from another map is rejected")
T.check(mismatchOw:restoreBattleContinuation(trainer, {
  kind = "trainer_encounter", map = "FIX_TOWN", npcId = trainerNpc.id,
  trainerClass = "OPP_OTHER", partyIndex = 1,
}) == false, "mismatched trainer identity is rejected")

local ow = {}
local stack = { states = { ow } }
function stack:top() return self.states[#self.states] end
local game = setmetatable({ overworld = ow, stack = stack }, { __index = GameMethods })
local entered, resumed = false, false
local battle = {
  enter = function() entered = true end,
  resumeCheckpoint = function() resumed = true end,
}
game:restoreCheckpointBattle(battle)
T.check(game.stack:top() == battle, "reconstructed battle is installed on stack")
T.check(resumed == true, "checkpoint-specific battle resume path runs")
T.check(entered == false, "ordinary battle intro is not replayed")

T.finish()
