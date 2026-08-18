-- Safe per-phase draw profiler (3DS).  NEVER throws: every file op is pcall-
-- wrapped and every timer access is guarded, so a missing module or a bug
-- here can never break the game (love.draw is never nil'd).
--
-- Usage at call sites (all guarded so DrawProf being nil is always safe):
--   if DrawProf then DrawProf.begin("phase") end
--   ...work...
--   if DrawProf then DrawProf.finish("phase") end
--   if DrawProf then DrawProf.frame() end   -- once per rendered frame
--
-- Publishes to cwd/drawprof.txt every FLUSH_EVERY frames: per-phase per-frame
-- average (ms) and single-frame peak (ms).  love.timer.getTime on the 3DS is
-- svcGetSystemTick/268123480 (microsecond resolution since R18), so both
-- averages and single-frame peaks are trustworthy.
local M = {}

local function now()
  if love and love.timer and love.timer.getTime then
    return love.timer.getTime() -- seconds
  end
  return 0
end

-- Flush cadence.  The flush is an SD create/write/close that runs inside the
-- timed love.draw window; on slow/aging cards a single write can stall 100ms+
-- and show up as a fake luaDraw spike.  Every 60 frames = a fresh sample every
-- ~2s but heavy SD pressure; 180 keeps ~6s samples (3s if we ever hit 60fps)
-- while cutting write pressure (and stutter chance) 3x.
local FLUSH_EVERY = 180

-- "_ow_"..phase global keys, cached so the per-frame publish allocates no
-- strings (GC churn inside the timed draw window is exactly what we profile).
local gkeys = {}
local function gkey(phase)
  local k = gkeys[phase]
  if not k then k = "_ow_" .. phase; gkeys[phase] = k end
  return k
end

local acc      -- phase -> accumulated ms this window
local peaks    -- phase -> max single-frame ms this window
local frameMs  -- phase -> this frame's ms (reset each frame)
local count
local lastFlushMs

local function reset()
  acc = {}
  peaks = {}
  frameMs = {}
  count = 0
  lastFlushMs = 0
end
reset()

M.begin = function(phase)
  local t = now() * 1000
  acc[phase] = (acc[phase] or 0) - t
  frameMs[phase] = (frameMs[phase] or 0) - t
end

M.finish = function(phase)
  local t = now() * 1000
  acc[phase] = (acc[phase] or 0) + t
  local f = (frameMs[phase] or 0) + t
  frameMs[phase] = f
  if f > (peaks[phase] or 0) then peaks[phase] = f end
  -- publish the last completed phase to a global for any C++ slow-frame line
  _G[gkey(phase)] = f
end

-- Mark the end of a rendered frame; flush averages+peaks every `every` frames
-- (default FLUSH_EVERY, see above).  Safe to call from a state's draw() so
-- only that state's frames are counted (no dilution from other states' frames).
M.frame = function(every)
  every = every or FLUSH_EVERY
  count = count + 1
  frameMs = {}
  if count % every ~= 0 then return end
  local lines = { string.format("frames=%d (avg ms/frame, peak ms)", count) }
  for k, v in pairs(acc) do
    lines[#lines + 1] = string.format("%s: avg=%.2f peak=%.2f",
                                      k, v / count, peaks[k] or 0)
  end
  table.sort(lines)
  local ok, f = pcall(io.open, "drawprof.txt", "w")
  if ok and f then
    local wrote = pcall(function()
      f:write(table.concat(lines, "\n") .. "\n")
      f:close()
    end)
    if not wrote then pcall(function() f:close() end) end
  end
  reset()
end

return M
