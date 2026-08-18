-- DrawSink: shared per-texture SpriteBatch sink.
--
-- HudTiles.tile and Font.drawCode both ultimately issue one
-- love.graphics.draw(image, quad, x, y) per tile/glyph.  On the 3DS each of
-- those is a Lua->C++ round trip, and the battle HUD (~70 tiles+glyphs a
-- frame) is the single biggest draw cost in a battle.  This module lets the
-- caller open a "sink" (one SpriteBatch per image); while it is open, tile
-- and glyph draws are recorded into the batches instead of being drawn.
-- The caller then draws each batch once and closes the sink.
--
-- Because this renderer applies no per-vertex tint (all C2D_DrawImageAt
-- calls pass NULL colorParams; image color comes from the texture pixels),
-- a single batch per texture is correct regardless of the current
-- love.graphics color -- black glyphs and white chrome tiles both come out
-- of their own pixels.
local DrawSink = {}

local activeSink = nil

-- The currently open sink (or nil).
function DrawSink.sink()
  return activeSink
end

function DrawSink.setSink(sink)
  activeSink = sink
end

-- A fresh, empty sink: a plain table keyed by image userdata -> SpriteBatch,
-- plus an `order` array so batches draw in insertion order (stable Z).
function DrawSink.newSink()
  return { order = {} }
end

-- Record one quad at (x, y) into the open sink.  Returns true when recorded
-- (caller should skip its direct draw), false when no sink is open.
function DrawSink.addToSink(img, quad, x, y)
  local sink = activeSink
  if not sink then return false end
  local b = sink[img]
  if not b then
    b = love.graphics.newSpriteBatch(img, 128)
    sink[img] = b
    sink.order[#sink.order + 1] = img
  end
  b:add(quad, x, y)
  return true
end

-- Draw every batch in a sink.  Safe to call when the sink has no entries.
function DrawSink.drawSink(sink)
  for i = 1, #sink.order do
    love.graphics.draw(sink[sink.order[i]], 0, 0)
  end
end

return DrawSink
