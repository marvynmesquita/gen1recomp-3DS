-- Engine invariant (#916): after the Fly / Dig departure animation ends, the
-- trainer sprite must stay hidden through the warp fade-out and only become
-- visible again when the arrival animation (flyArrive / teleport spin-down)
-- plays on the new map.
--
-- Root cause: the player-hide guard only held while a departure animation
-- was live.  flyAnim was nil'd the instant the bird finished path2, and the
-- teleportOut countdown cleared the spin fields at 0, but startWarpTo's
-- 32-frame fade keeps the overworld drawing beneath the veil (the Transition
-- is not isOpaque), so with the departure guard gone and the arrival not yet
-- armed, the standing sprite popped back in at the old cell for the whole
-- fade.
--
-- The fix is a playerHidden flag on OverworldState: set when the departure
-- completes (flyAnim path2 / teleportOut countdown), cleared in startWarpTo's
-- midpoint the same tick the arrival arms, and folded into both player-draw
-- guards.  This suite runs the REAL Transition + setMap headlessly and
-- asserts there is no fade frame where the player would draw bare.
--
-- ROM-free (fixture dataset, no ROM boot): lives in tests/engine so the CI
-- headless tier runs it; also runnable standalone via
-- `luajit tests/engine/warp_sprite_hidden_bug916.lua`.

package.path = "./?.lua;./?/init.lua;" .. package.path
if not _G.love then _G.love = require("tests.love_stub") end

local T = require("tests.modkit")
local check, eq = T.check, T.eq

local Data = T.fixtures.fresh()
-- fixture patches that let the overworld boot and run headlessly
Data.tilesets.FIX_OUT.tilesPerRow = 16
Data.field.flyWarps = Data.field.flyWarps or {}
Data.field.playerSprites = { walk = "SPRITE_FIX_PLAYER" }
Data.field.waterTilesets = {}
Data.field.forcedMovement = { tiles = {} }

local Game       = require("src.core.Game")
local Input      = require("src.core.Input")
local StateStack = require("src.core.StateStack")
local Renderer   = require("src.render.Renderer")
local SaveData   = require("src.core.SaveData")
local Pokemon    = require("src.pokemon.Pokemon")
local OW         = require("src.world.OverworldController")

Game.data = Data
Game.input = Input; Input:init()
Game.renderer = Renderer; Renderer:init()
Game.stack = StateStack; StateStack:init()
Game.save = SaveData.newGame()
Game.save.party = { Pokemon.new(Data, "FIXMON_A", 20) }
local stack = Game.stack

-- The draw guard both entity passes use: the player sprite is skipped while
-- any of flyAnim / flyArrive / playerHidden is set.
local function playerHidden(ow)
  return ow.flyAnim ~= nil or ow.flyArrive ~= nil or ow.playerHidden == true
end

local function newOW()
  stack:push(OW, "FIX_TOWN", 5, 6, "down")
  local ow = stack:top()
  Game.overworld = ow
  return ow
end

-- Drive `ow` until its departure + warp + arrival all complete, tracking the
-- fade window.  Returns counters: fadeFrames / fadeFramesHidden (frames the
-- Transition was on top; of those, frames the player was hidden), gapFrames
-- (fade frames where NO arrival was active AND the player was NOT hidden --
-- the regression this suite guards), arrivalFrame (first frame an arrival
-- animation armed), warpFrame (first frame a fade is up).
--
-- Breaks once an arrival armed and then fully finished (no stale departure
-- or arrival animation, OW back on top); `maxFrames` is the safety net.
local function drive(ow, maxFrames)
  local st = { fadeFrames = 0, fadeFramesHidden = 0, gapFrames = 0,
               arrivalFrame = nil, warpFrame = nil }
  for i = 1, maxFrames or 260 do
    local fading = stack:top() ~= ow
    stack:update()
    if fading then
      st.fadeFrames = st.fadeFrames + 1
      if playerHidden(ow) then st.fadeFramesHidden = st.fadeFramesHidden + 1 end
      local arrivalActive = ow.flyArrive ~= nil or ow.player.spinDrop == true
      if not arrivalActive and not playerHidden(ow) then
        st.gapFrames = st.gapFrames + 1
      end
      if st.warpFrame == nil then st.warpFrame = i end
    end
    if st.arrivalFrame == nil
        and (ow.flyArrive ~= nil or ow.player.spinDrop == true) then
      st.arrivalFrame = i
    end
    if st.arrivalFrame and stack:top() == ow
        and ow.flyArrive == nil and ow.player.spinDrop ~= true
        and not ow.player.inputLocked then
      break -- departure + fade + arrival all finished
    end
  end
  return st
end

-- ------------------------------------------------------------------ dig/teleport
-- Departure spin (48) -> warp fade -> arrival spin-down.  From the moment
-- the spin ends until the arrival arms, the sprite must never draw bare.
local ow = newOW()
local doneFired = false
ow:beginTeleportOut(function()
  doneFired = true
  ow.player.inputLocked = false -- the party-menu caller unlocks after the warp
end)
local st = drive(ow, 260)
check(st.warpFrame ~= nil, "dig departure ends and the warp fade begins")
check(st.fadeFrames > 0, "dig warp fade ran (" .. st.fadeFrames .. " frames)")
eq(st.gapFrames, 0,
   "no dig fade frame leaves the player standing bare (#916)")
check(st.fadeFramesHidden >= st.fadeFrames - 1,
       "dig fade hidden on every frame but the arrival-arming midpoint ("
       .. st.fadeFramesHidden .. "/" .. st.fadeFrames .. ")")
check(st.arrivalFrame ~= nil, "dig arrival spin-down arms")
check(ow.playerHidden == false, "dig hide cleared on the new map")
check(doneFired, "dig onDone fires after the warp")
check(ow.player.spinDrop ~= true and ow.player.spinning == false,
      "dig arrival spin-down completes")
check(not playerHidden(ow), "player drawable again after the dig landing")

-- ------------------------------------------------------------------ fly
-- flap (24) + path1 (36) + hold (40) + path2 (33) = 133 frames of flyAnim,
-- then the fade, then the bird swoops in (flyArrive).  Same invariant.
Data.field.flyWarps.FIX_ROUTE = { x = 4, y = 6 }
ow = newOW()
ow:flyTo("FIX_ROUTE")
st = drive(ow, 260)
check(st.warpFrame ~= nil, "fly departure ends and the warp fade begins")
-- flap (8*3) + path1 (12*3) + hold (40) + path2 (11*3) = 133 frames; the
-- warp fires on frame 133's update, so the fade is on top from loop frame 134
eq(st.warpFrame, 134, "fly fade begins right after the bird''s exit path")
check(st.fadeFrames > 0, "fly warp fade ran (" .. st.fadeFrames .. " frames)")
eq(st.gapFrames, 0,
   "no fly fade frame leaves the player standing bare (#916)")
check(st.fadeFramesHidden >= st.fadeFrames - 1,
       "fly fade hidden on every frame but the arrival-arming midpoint ("
       .. st.fadeFramesHidden .. "/" .. st.fadeFrames .. ")")
check(st.arrivalFrame ~= nil, "fly arrival swoop arms")
check(ow.playerHidden == false, "fly hide cleared on the new map")
check(ow.flyArrive == nil, "fly arrival swoop completes")
check(not ow.player.inputLocked, "fly landing releases player input")
check(not playerHidden(ow), "player drawable again after the fly landing")

T.finish("warp_sprite_hidden_bug916")
