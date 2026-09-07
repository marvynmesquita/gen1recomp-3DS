-- ChipAudio synthesis worker (love.thread).  Runs the Game Boy audio synth
-- (src/core/ChipSynth.lua) off the main thread so a map/battle song change
-- never stutters the render thread: filling the ~6s playback queue from
-- scratch is ~200ms of Lua synthesis, and this is where it now happens.
--
-- Protocol -- main thread pushes command tables onto the "chipaudio_cmd"
-- channel and drains produced buffers off "chipaudio_out":
--   cmd = "play"  { gen, header, allowLoops, audio,
--                   channelVolumes?, channelPitches? }
--   cmd = "stop"                                       halt production
--   cmd = "channelMix" { volumes, pitches }            per-hw volume/pitch
--   cmd = "invalidate"                                 drop the bank cache
--   cmd = "quit"                                        end the thread
-- out buffers are tagged with the play's `gen` so the main thread can
-- discard anything left over from a superseded song:
--   { gen, sd = SoundData }   one rendered buffer
--   { gen, done = true }      the song ended (non-looping jingle finished)
--   { gen, error = msg }      build/synth failed; main logs and gives up

require("love.thread")
require("love.timer")
require("love.sound")
require("love.filesystem")

-- Load the synth explicitly via love.filesystem (a fresh thread Lua state does
-- not necessarily carry the package searcher that resolves "src.core..."):
local ChipSynth = assert(love.filesystem.load("src/core/ChipSynth.lua"))()

local cmdCh = love.thread.getChannel("chipaudio_cmd")
local outCh = love.thread.getChannel("chipaudio_out")

local BUF = ChipSynth.MUSIC_BUFFER_SAMPLES
-- how many finished buffers may sit in the hand-off channel before the worker
-- pauses.  The deep (~3s) playback depth lives in the main-thread Source; this
-- only bounds the worker's look-ahead (and its memory) between drains.
local LOOKAHEAD = 16

local gen = nil        -- active song generation, or nil when stopped
local engine = nil     -- the ChipSynth engine producing the current song
local finished = false -- the current song ran out (non-looping)
local data = nil       -- { audio = <slim audio tables> } for ROM bank/wave reads

local function handle(cmd)
  if cmd.cmd == "play" then
    gen = cmd.gen
    finished = false
    engine = nil
    outCh:clear() -- drop any buffers left from the previous song
    data = { audio = cmd.audio }
    if cmd.channelVolumes ~= nil then
      ChipSynth.setChannelVolumes(cmd.channelVolumes)
    end
    if cmd.channelPitches ~= nil then
      ChipSynth.setChannelPitches(cmd.channelPitches)
    end
    if cmd.stereo ~= nil then
      ChipSynth.setStereo(cmd.stereo)
    end
    local ok, eng = pcall(ChipSynth.newEngine, data, cmd.header,
                          { allowLoops = cmd.allowLoops })
    if ok then
      engine = eng
    else
      local emsg = tostring(eng)
      local etb = debug.traceback(emsg, 2) or "no traceback"
      -- a failed push must never kill the thread (the main side will just log
      -- it); pcall the formatter so OOM inside string.format can't take the
      -- worker down and leave the game permanently silent.
      pcall(outCh.push, outCh, "{ gen=" .. tostring(gen) .. ", error="
        .. string.format("%q", tostring(emsg)) .. ", traceback="
        .. string.format("%q", tostring(etb)) .. " }")
      finished = true
    end
  elseif cmd.cmd == "stop" then
    gen = nil
    engine = nil
    finished = false
    outCh:clear()
  elseif cmd.cmd == "channelMix" then
    if cmd.volumes ~= nil then ChipSynth.setChannelVolumes(cmd.volumes) end
    if cmd.pitches ~= nil then ChipSynth.setChannelPitches(cmd.pitches) end
    if cmd.stereo ~= nil then ChipSynth.setStereo(cmd.stereo) end
  elseif cmd.cmd == "invalidate" then
    ChipSynth.invalidateBanks()
  elseif cmd.cmd == "quit" then
    return true
  end
  return false
end

while true do
  -- drain every pending command first, so a stop/new-play is seen promptly
  local quit = false
  local cmdStr = cmdCh:pop()
  while cmdStr do
    local f = loadstring("return " .. cmdStr)
    if f then
      local okH, hres = pcall(handle, f())
      if okH then
        if hres then quit = true end
      else
        pcall(outCh.push, outCh, "{ gen=" .. tostring(gen)
          .. ", error=" .. string.format("%q",
            "worker handle error: " .. tostring(hres)) .. " }")
        -- a malformed command is fatal to this song: stop the engine so we
        -- don't spin on it forever
        finished = true
      end
    end
    cmdStr = cmdCh:pop()
  end
  if quit then break end

  if engine and not finished and gen and outCh:getCount() < LOOKAHEAD then
    local activeGen = gen
    local ok, sd = pcall(ChipSynth.soundData, engine, BUF, 2)
    if not ok then
      local msg = tostring(sd)
      local tb = debug.traceback(msg, 2) or "no traceback"
        pcall(outCh.push, outCh, "{ gen=" .. tostring(activeGen)
          .. ", error=" .. string.format("%q", tostring(msg))
          .. ", traceback=" .. string.format("%q", tostring(tb)) .. " }")
      finished = true
    else
      outCh:push("{ gen=" .. activeGen .. ", sd=true }")
      outCh:push(sd)
      if engine:finished() then
        outCh:push("{ gen=" .. activeGen .. ", done=true }")
        finished = true
      end
    end
  else
    -- nothing to do (idle, or the look-ahead is full): yield the core
    love.timer.sleep(0.001)
  end
end
