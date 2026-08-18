-- Native 3DS two-screen launcher with runtime ROM import.
--
-- Top screen (400x240, glasses-free 3D): the Gen1Recomp logo POPS OUT in
-- front (strong positive _parallax, like the game's P_MENU / P_FRONT layers)
-- over a Gen-1-themed solid background with decorative pixel sprites on
-- NEGATIVE (recessed, behind-the-screen) layers, so the parallax barrier
-- separates them into comfortable depth.  Convention matches the game:
-- positive = in front of the screen, negative = behind it.
--
-- Bottom screen (320x240, always mono): a GAME button opens the list of
-- compatible ROMs found in sdmc:/3ds/gen1recomp3ds/roms; MODS is a
-- placeholder for the future mods/ work; START boots the selected game -
-- extracting its cache from the ROM to the SD card first, when that version
-- has not been imported yet.  Red always boots from the romfs-baked cache
-- and never needs a ROM file.  The loading states just show the pokeball
-- swaying (no ring / bar).
--
-- During gameplay the bottom screen is blank, except for the small launcher
-- menu that L+R+START opens (return to the launcher / keep playing).
--
-- Only active on the native 3DS build (detected by the _listdir C bridge).
local M = {}

local GameVersion = require("src.core.GameVersion")
local CacheFs = require("src.import.CacheFs")

local CACHE_FORMAT = "rom-cache-v10:"

-- main.lua handlers, reinstated when a game boots.  They are captured ONCE
-- (pristineCaptured) so re-entering M.boot from the in-game menu cannot chain
-- the L+R+START wrappers (installed by boot) onto themselves.
local savedUpdate, savedDraw
local pristineUpdate, pristineDraw
local savedKeypressed, savedKeyreleased, savedGamepadpressed
local pristineCaptured = false
local bootGameFn

local state = "menu"          -- "menu" | "romlist" | "scanning" | "importing" | "booting" | "settings"
local roms = {}               -- { name, version, ready, data }
local selectedIdx = 1
local notice = ""
local noticeUntil = 0

-- ---- settings (gear menu) ------------------------------------------------
local SETTINGS_FILE = "sdmc:/3ds/gen1recomp3ds/options.lua"
local settings = {
  textSpeed    = 3,        -- 1=FAST, 3=MEDIUM, 5=SLOW
  animations   = true,
  battleStyle  = "shift",  -- "shift" / "set"
  battleLayout = "og",     -- "og" / "wide"
  battleFit    = "fixed",  -- "fixed" / "fill"
  battleBg     = "white",  -- "white" / "black" / "world"
  uiLayout     = "centered", -- "centered" / "dynamic"
  musicVol     = 7,        -- 0-7
  sfxVol       = 7,        -- 0-7
  pikaVol      = 7,        -- 0-7 (Yellow only)
  musicFilter  = 0,        -- 0=OFF, 1=1X, 2=2X, 3=3X
  colors       = "gbc",    -- "ogred","gbc","redpp","og","og_inv","gbc_inv","classic"
  tilt         = 0,        -- 0=OFF, 1=15, 2=35, 3=50
  performance  = "auto",   -- "auto" / "high" / "balanced" / "low"
  speedOverworld = 1,
  speedBattle  = 1,
  speedMenu    = 1,
  zoom         = 0,
  voidFill     = "trees",  -- "trees" / "water" / "black"
  fpsCap       = 60,
}
local settingsIdx = 1     -- focused row in the settings list
local settingsScroll = 1  -- first visible row when list > screen

-- SETTINGS_ROWS: every row shown in the gear menu.  "section" rows are
-- non-interactive headers that group the list visually.
local SETTINGS_ROWS = {
  -- SECTION: Texto & Velocidade
  { section = "TEXTO & VELOCIDADE" },
  { key = "textSpeed",      label = "VELOCIDADE TEXTO",  type = "choice",
    choices = { {1,"RAPIDO"}, {3,"MEDIO"}, {5,"LENTO"} } },
  { key = "speedOverworld", label = "VELOC. MAPA",       type = "number",
    min = 1, max = 5 },
  { key = "speedBattle",    label = "VELOC. BATALHA",    type = "number",
    min = 1, max = 5 },
  { key = "speedMenu",      label = "VELOC. MENU",       type = "number",
    min = 1, max = 5 },

  -- SECTION: Batalha
  { section = "BATALHA" },
  { key = "animations",     label = "ANIMACAO",          type = "toggle" },
  { key = "battleStyle",    label = "ESTILO BATALHA",    type = "choice",
    choices = { {"shift","SHIFT"}, {"set","SET"} } },
  { key = "battleLayout",   label = "LAYOUT BATALHA",    type = "choice",
    choices = { {"og","ORIGINAL"}, {"wide","WIDE"} } },
  { key = "battleFit",      label = "TAM. BATALHA",      type = "choice",
    choices = { {"fixed","FIXED"}, {"fill","FILL"} } },
  { key = "battleBg",       label = "FUNDO BATALHA",     type = "choice",
    choices = { {"white","BRANCO"}, {"black","PRETO"}, {"world","MAPA"} } },

  -- SECTION: Interface
  { section = "INTERFACE" },
  { key = "uiLayout",       label = "LAYOUT UI",         type = "choice",
    choices = { {"centered","CENTRO"}, {"dynamic","DINAMICO"} } },
  { key = "voidFill",       label = "PREENCH. MAPA",     type = "choice",
    choices = { {"trees","ARVORES"}, {"water","AGUA"}, {"black","PRETO"} } },

  -- SECTION: Audio
  { section = "AUDIO" },
  { key = "musicVol",       label = "VOL. MUSICA",       type = "number",
    min = 0, max = 7 },
  { key = "sfxVol",         label = "VOL. EFEITOS",      type = "number",
    min = 0, max = 7 },
  { key = "pikaVol",        label = "VOL. PIKACHU",      type = "number",
    min = 0, max = 7 },
  { key = "musicFilter",    label = "FILTRO MUSICA",     type = "choice",
    choices = { {0,"OFF"}, {1,"1X"}, {2,"2X"}, {3,"3X"} } },

  -- SECTION: Visual
  { section = "VISUAL" },
  { key = "colors",         label = "CORES",             type = "choice",
    choices = { {"ogred","RED original"}, {"gbc","GBC"},
                {"redpp","RED++"}, {"og","OG mono"},
                {"og_inv","OG invert"}, {"gbc_inv","GBC invert"},
                {"classic","CLASSIC"} } },
  { key = "tilt",           label = "TILT",              type = "choice",
    choices = { {0,"OFF"}, {1,"15"}, {2,"35"}, {3,"50"} } },
  { key = "performance",    label = "DESEMPENHO",        type = "choice",
    choices = { {"auto","AUTO"}, {"high","ALTO"},
                {"balanced","EQUILIBRADO"}, {"low","BAIXO"} } },

  -- SECTION: Avancado
  { section = "AVANCADO" },
  { key = "zoom",           label = "ZOOM",              type = "number",
    min = 0, max = 5 },
  { key = "fpsCap",         label = "FPS MAX",           type = "choice",
    choices = { {30,"30"}, {60,"60"} } },
}
local gearIcon = nil

local function loadSettings()
  local f = io.open(SETTINGS_FILE, "r")
  if not f then return end
  local data = f:read("*a")
  f:close()
  if not data or data == "" then return end
  -- Simple line-based: key=value
  for line in data:gmatch("[^\n]+") do
    local k, v = line:match("^([%w]+)=(.+)$")
    if k and settings[k] ~= nil then
      if type(settings[k]) == "boolean" then
        settings[k] = (v == "true")
      elseif type(settings[k]) == "number" then
        settings[k] = tonumber(v) or settings[k]
      else
        settings[k] = v
      end
    end
  end
end

local function saveSettings()
  local f = io.open(SETTINGS_FILE, "w")
  if not f then return end
  for _, row in ipairs(SETTINGS_ROWS) do
    local v = settings[row.key]
    if type(v) == "boolean" then
      f:write(row.key .. "=" .. tostring(v) .. "\n")
    else
      f:write(row.key .. "=" .. tostring(v) .. "\n")
    end
  end
  f:close()
end

local import = { active = false, done = false, progress = 0, status = "",
                 version = nil, co = nil, rom = nil, yields = 0 }

-- Asynchronous ROM scan (GAME button): runs as a coroutine that yields
-- before each ROM read, so the press-flash + loading ring keep painting
-- between reads instead of the whole scan blocking the frame for ~16 s (the
-- old synchronous scanRoms froze the screen with zero feedback).
local scan = { active = false, co = nil, current = 0, total = 0, result = nil,
               goTo = "menu" }

-- Focused menu button for physical D-pad + A/B navigation (1=GAME, 2=MODS,
-- 3=START).  Touch users ignore it; it is drawn as a thin border.
local focusIdx = 1
local romListScroll = 1  -- first visible ROM in list

-- Brief "the tap registered" flash, set by handleTouch on each button press.
local pressFx = { id = nil, pressUntil = 0 }

local logo, logoW, logoH
local pallet, palletW, palletH
local pokeball, pokeballW, pokeballH

-- ----- helpers ---------------------------------------------------------------

local function sha1Hex(data)
  local digest = love.data.hash("sha1", data)
  if type(digest) == "userdata" and digest.getString then
    digest = digest:getString()
  end
  return love.data.encode("string", "hex", digest)
end

-- Red's cache is baked into romfs: always bootable, no ROM file needed.
-- Other versions count as ready only when their SD cache carries the
-- completion marker (written last by the import, so presence == complete).
local function versionReady(version)
  local info = GameVersion.info(version)
  local saved = CacheFs.prefix
  CacheFs.prefix = info.cachePrefix
  local marker = CacheFs.read("rom-cache.complete")
  CacheFs.prefix = saved
  return marker == (CACHE_FORMAT .. info.sha1)
end

local function setNotice(text)
  notice = text
  noticeUntil = love.timer.getTime() + 2.5
end

-- Async ROM scan.  Yields before each ROM read so the bottom screen keeps
-- drawing the loading ring between reads; launcherUpdate drives it and moves
-- to "romlist" when it finishes.  scan.result holds the sorted list.
local function startScan(goTo)
  scan.active = true
  scan.current = 0
  scan.total = 0
  scan.result = nil
  scan.goTo = goTo or "romlist"
  scan.co = coroutine.create(function()
    local entries = _listdir("roms") or {}
    local count = 0
    for _, name in ipairs(entries) do
      if name:lower():match("%.gbc?$") then count = count + 1 end
    end
    scan.total = count
    coroutine.yield()   -- let the ring paint before the first read
    local out = {}
    local n = 0
    for _, name in ipairs(entries) do
      if name:lower():match("%.gbc?$") then
        n = n + 1
        scan.current = n
        coroutine.yield()   -- render the ring between ROMs
        local data = love.filesystem.read("roms/" .. name)
        if data then
          local version = GameVersion.forSha1(sha1Hex(data))
          local ready = version and versionReady(version) or false
          out[#out + 1] = {
            name = name,
            version = version,
            ready = ready,
            -- Keep the ROM bytes only when an import may still need them:
            -- ready versions boot from cache and must not pin MBs of ROM in
            -- memory while the game loads.
            data = ready and nil or data,
          }
        end
      end
    end
    -- NOTE: Built-in Red removed — only ROMs found on SD are listed.
    table.sort(out, function(a, b) return a.name < b.name end)
    scan.result = out
    scan.active = false
  end)
  state = "scanning"
end

local function hit(px, py, x, y, w, h)
  return px >= x and px <= x + w and py >= y and py <= y + h
end

-- ----- import ---------------------------------------------------------------

local boot -- forward declaration (defined below, called by importUpdate)
local scheduleBoot -- forward declaration (booting state, defined below)
-- In-game L+R+START menu (defined below, installed by boot):
local gameUpdate, gameKeypressed, gameKeyreleased
local openGameMenu, closeGameMenu, exitToLauncher
local heldL, heldR, gameMenu, gameMenuChoice

local function startImport(rom)
  local info = GameVersion.info(rom.version)
  local manifestRaw = love.filesystem.read(info.manifest)
  if not manifestRaw then
    setNotice("Error: manifest for " .. info.displayName .. " missing")
    state = "menu"
    return
  end
  local Json = require("src.link.Json")
  local manifest = Json.decode(manifestRaw)
  if not manifest then
    setNotice("Error: invalid manifest")
    state = "menu"
    return
  end
  local RomExtractor = rom.version == "gold"
      and require("src.import.RomExtractorGen2")
      or require("src.import.RomExtractor")
  -- Ensure the selected ROM's bytes are resident before we commit to the
  -- import: a previous import's cleanup may have dropped them (or the
  -- built-in Red entry has none and boots straight from its baked cache).
  if not rom.data then
    rom.data = love.filesystem.read("roms/" .. rom.name)
    if not rom.data then
      setNotice("Error: could not read " .. rom.name)
      state = "menu"
      return
    end
  end
  state = "importing"
  notice = ""
  import.active = true
  import.done = false
  import.progress = 0
  import.yields = 0
  import.status = "Verifying " .. info.displayName
  import.version = rom.version
  import.rom = rom
  -- Free every other ROM's bytes now: the scan pins each not-ready ROM's
  -- data (~1 MiB each), and with several carts on the SD card that is several
  -- MB the extractor badly needs for the sprite/trainer stages (12-13).  Only
  -- the ROM being imported stays resident; any other ROM is re-read from the
  -- SD card the moment it is selected for import.
  for _, r in ipairs(roms) do
    if r ~= rom then r.data = nil end
  end
  collectgarbage("collect")
  print(string.format("[import] start %s (heap %.1f MB)",
                      info.displayName, collectgarbage("count") / 1024))
  import.co = coroutine.create(function()
    CacheFs.prefix = info.cachePrefix
    local extractor = RomExtractor.new(rom.data, manifest,
      function(p, total, stage, current, stageTotal)
        import.progress = p / total
        import.status = stage
        coroutine.yield()
      end)
    extractor:run()
    local ok, werr = CacheFs.write("rom-cache.complete",
                                   CACHE_FORMAT .. info.sha1)
    CacheFs.prefix = ""
    if not ok then error("could not finish the private cache: " .. tostring(werr)) end
    import.done = true
  end)
end

local function importUpdate()
  if not import.active then return end
  local ok, err = coroutine.resume(import.co)
  if not ok then
    import.active = false
    import.status = ""
    state = "menu"
    setNotice("Import error: " .. tostring(err))
    print("[import] FAILED: " .. tostring(err))
    return
  end
  -- Keep the Lua heap bounded while the extractor allocates megabytes of
  -- image/text strings: Lua 5.1's non-incremental GC reclaims lazily, and an
  -- unchecked heap can exhaust the 3DS and hard-crash.  Force a collection
  -- every so often (cheap at these frequencies; the extraction dominates).
  import.yields = import.yields + 1
  if import.yields % 30 == 0 then
    collectgarbage("collect")
  end
  if coroutine.status(import.co) == "dead" then
    import.active = false
    if import.done then
      local version = import.version
      import.rom = nil
      import.status = ""
      -- Free every ROM file the launcher still holds, then collect the
      -- extractor's result garbage, so the game boots with a clean heap.
      for _, r in ipairs(roms) do r.data = nil end
      collectgarbage("collect")
      print(string.format("[import] done %s (heap %.1f MB)",
                          GameVersion.info(version).displayName,
                          collectgarbage("count") / 1024))
      scheduleBoot(version)
    end
  end
end

-- ----- boot -----------------------------------------------------------------

boot = function(version)
  local info = GameVersion.info(version)
  local prefix = info.cachePrefix
  _activeCachePrefix = prefix
  -- Inject launcher settings into the game before it loads
  _launcherSettings = {}
  for _, row in ipairs(SETTINGS_ROWS) do
    if row.key then
      _launcherSettings[row.key] = settings[row.key]
    end
  end
  -- Make require() resolve generated modules from the version's SD cache.
  if prefix ~= "" and prefix ~= "red/" then
    package.path = "sdmc:/3ds/gen1recomp3ds/" .. prefix ..
      "?.lua;sdmc:/3ds/gen1recomp3ds/" .. prefix ..
      "?/init.lua;" .. package.path
  end
  -- Free launcher ROM bytes so the game's boot has maximum free heap.
  for _, r in ipairs(roms) do r.data = nil end
  -- Save the launcher's own handlers before bootGameFn overwrites them.
  -- These are needed by exitToLauncher() to return to the launcher.
  heldL, heldR, gameMenu, gameMenuChoice = false, false, false, 2
  local launcherUpdate_save = love.update
  local launcherDraw_save = love.draw
  local launcherKeypressed_save = love.keypressed
  -- Boot the game — this calls bootGameFn which calls Game:load().
  -- NOTE: bootGameFn does NOT set love.update/love.draw — the game defines
  -- them as top-level functions in main.lua, but M.boot() overwrote them
  -- with the launcher handlers.  Restore them from the pristine copies
  -- captured on the first M.boot() call.
  -- Safety: ensure no stale generated data from a previous game is in the
  -- require cache.  exitToLauncher() already does this, but belt-and-suspenders
  -- protects against cold-boot edge cases.
  pcall(function() require("src.core.Data"):unloadGenerated() end)
  print("[boot] calling bootGameFn(" .. tostring(version) .. ")")
  if bootGameFn then
    local ok, err = pcall(bootGameFn, version)
    if not ok then
      print("[boot] bootGameFn FAILED: " .. tostring(err))
      state = "menu"
      setNotice("Boot failed: " .. tostring(err))
      return
    end
  end
  print("[boot] game booted OK")
  -- Restore the game's own love.update and love.draw from main.lua.
  if pristineUpdate then love.update = pristineUpdate end
  if pristineDraw then love.draw = pristineDraw end
  -- NOW capture the game's live handlers so exitToLauncher() can restore
  -- them on the next return.
  savedUpdate = love.update
  savedDraw = love.draw
  -- Install the game-menu wrappers.  These call saved* (the game's real
  -- handlers) for key input, and only intercept when the L+R+START menu
  -- is open.
  love.keypressed = gameKeypressed
  love.keyreleased = gameKeyreleased
  love.gamepadpressed = savedGamepadpressed or (function() end)
  love.graphics._drawBottom = nil
  -- Install the update wrapper that pauses the game when the menu is open.
  love.update = gameUpdate
end

-- ----- booting state ---------------------------------------------------------
-- The game's boot (Data:load reads + compiles ~2MB of generated cache from
-- SD) is a multi-second synchronous block.  Pressing START on a ready ROM
-- therefore cannot call boot() from the tap handler, or the menu would freeze
-- in place for the whole load with zero feedback.  Instead we drop into a
-- short "booting" state that paints a Carregando ring for a few frames, then
-- hands off to boot(), which swaps in the game's own update/draw (and splash).
local bootPending, bootAt

scheduleBoot = function(version)
  bootPending = version
  bootAt = love.timer.getTime() + 0.35
  state = "booting"
end

local function bootPendingUpdate()
  if not bootPending then return end
  if love.timer.getTime() < (bootAt or 0) then return end
  local version = bootPending
  bootPending = nil
  local ok, err = pcall(boot, version)
  if not ok then
    state = "menu"
    setNotice("Boot failed: " .. tostring(err))
    print("[boot] FAILED: " .. tostring(err))
  end
end

-- ----- controls -------------------------------------------------------------

local MENU_BUTTONS = {
  { id = "jogo",    x = 30, y = 88,  w = 260, h = 38, label = "GAME" },
  { id = "mods",    x = 30, y = 134, w = 260, h = 38, label = "MODS" },
  { id = "iniciar", x = 30, y = 180, w = 260, h = 38, label = "START" },
}

local function onStart()
  local sel = roms[selectedIdx]
  if not sel then
    setNotice("No ROM. Tap GAME.")
    return
  end
  if not sel.version then
    setNotice("Incompatible ROM: " .. sel.name)
    return
  end
  if sel.ready then
    scheduleBoot(sel.version)
    return
  end
  if not sel.data then
    setNotice("ROM has no data: " .. sel.name)
    return
  end
  startImport(sel)
end

-- Shared button action for touch taps and physical A/START presses: flashes
-- the button, then runs the action.  The ROMs are scanned once at boot (the
-- scan verifies each ROM's SHA-1 against the known versions), so GAME just
-- opens that list -- no second scan; imports run in their own coroutine, so
-- the flash and the loading ring still paint before the heavy work.
local function pressButton(id)
  pressFx.id = id
  pressFx.pressUntil = love.timer.getTime() + 0.18
  if id == "jogo" then
    romListScroll = 1
    state = "romlist"
  elseif id == "mods" then
    setNotice("MODS: coming soon!")
  elseif id == "iniciar" then
    onStart()
  end
end

local function handleTouch()
  if not love.touch.getPressed() then return end
  local px, py = love.touch.getPosition()
  if state == "menu" then
    -- Gear icon touch (top-right corner)
    if gearIcon and hit(px, py, 292, 4, 24, 24) then
      state = "settings"
      settingsIdx = 1
      return
    end
    for _, b in ipairs(MENU_BUTTONS) do
      if hit(px, py, b.x, b.y, b.w, b.h) then
        pressButton(b.id)
        return
      end
    end
  elseif state == "settings" then
    -- Touch on settings rows: find which row was hit by scanning visible positions
    local drawY = SETTINGS_LIST_TOP
    for i = settingsScroll, #SETTINGS_ROWS do
      local row = SETTINGS_ROWS[i]
      local h = row.section and SETTINGS_SECTION_H or SETTINGS_ROW_H
      if drawY + h > 214 then break end
      if hit(px, py, 10, drawY, 300, h) then
        if row.key then
          settingsIdx = i
          -- Toggle or cycle the value
          local dir = 1
          if row.type == "toggle" then
            settings[row.key] = not settings[row.key]
          elseif row.type == "choice" then
            local cur = settings[row.key]
            for ci, cv in ipairs(row.choices) do
              if cv[1] == cur then
                local ni = ci + 1
                if ni > #row.choices then ni = 1 end
                settings[row.key] = row.choices[ni][1]
                break
              end
            end
          elseif row.type == "number" then
            settings[row.key] = settings[row.key] + 1
            if settings[row.key] > row.max then settings[row.key] = row.min end
          end
          saveSettings()
        end
        do return end  -- exit both loops
      end
      drawY = drawY + h
    end
    -- BACK button (top-left) in settings screen
    if hit(px, py, 10, 6, 48, 20) then
      state = "menu"
      return
    end
    return
  elseif state == "romlist" then
    -- BACK button (top-left)
    if hit(px, py, 10, 6, 48, 20) then
      state = "menu"
      return
    end
    -- ROM rows: map touch Y to visible scroll position
    local visibleCount = 7
    local rowH = 26
    local listTop = 36
    for vi = 0, visibleCount - 1 do
      local i = romListScroll + vi
      if i > #roms then break end
      local y = listTop + vi * rowH
      if hit(px, py, 10, y, 300, rowH - 2) then
        if selectedIdx == i then
          -- Double-tap: confirm selection
          state = "menu"
        else
          selectedIdx = i
        end
        return
      end
    end
  end
end

-- Physical 3DS controls for the launcher (D-pad + A/B + START):
--   menu:    D-pad up/down cycles the GAME/MODS/START focus; A or START
--            presses the focused button.
--   romlist: D-pad up/down moves the selection; A confirms (back to the menu
--            with that ROM selected); B goes back to the menu.
--   scanning/importing: inputs ignored (the action is already running).
local function launcherKeypressed(key)
  if state == "menu" then
    if key == "up" then
      focusIdx = focusIdx - 1
      if focusIdx < 1 then focusIdx = #MENU_BUTTONS end
    elseif key == "down" then
      focusIdx = focusIdx + 1
      if focusIdx > #MENU_BUTTONS then focusIdx = 1 end
    elseif key == "z" or key == "kpenter" then
      pressButton(MENU_BUTTONS[focusIdx].id)
    end
  elseif state == "romlist" then
    if key == "up" then
      selectedIdx = selectedIdx - 1
      if selectedIdx < 1 then selectedIdx = 1 end
    elseif key == "down" then
      selectedIdx = selectedIdx + 1
      if selectedIdx > #roms then selectedIdx = #roms end
    elseif key == "z" or key == "kpenter" then
      -- A on selected ROM: go to menu with it selected
      state = "menu"
    elseif key == "x" then
      state = "menu"
    end
  elseif state == "settings" then
    if key == "up" then
      settingsIdx = settingsIdx - 1
      if settingsIdx < 1 then settingsIdx = #SETTINGS_ROWS end
      -- skip section-only rows (no key)
      while settingsIdx > 1 and not SETTINGS_ROWS[settingsIdx].key do
        settingsIdx = settingsIdx - 1
      end
    elseif key == "down" then
      settingsIdx = settingsIdx + 1
      if settingsIdx > #SETTINGS_ROWS then settingsIdx = 1 end
      while settingsIdx <= #SETTINGS_ROWS and not SETTINGS_ROWS[settingsIdx].key do
        settingsIdx = settingsIdx + 1
      end
      if settingsIdx > #SETTINGS_ROWS then settingsIdx = 1 end
    elseif key == "z" or key == "kpenter" then
      -- A: toggle/cycle value
      local row = SETTINGS_ROWS[settingsIdx]
      if row and row.key then
        if row.type == "toggle" then
          settings[row.key] = not settings[row.key]
        elseif row.type == "choice" then
          local cur = settings[row.key]
          for ci, cv in ipairs(row.choices) do
            if cv[1] == cur then
              local ni = ci + 1
              if ni > #row.choices then ni = 1 end
              settings[row.key] = row.choices[ni][1]
              break
            end
          end
        elseif row.type == "number" then
          settings[row.key] = settings[row.key] + 1
          if settings[row.key] > row.max then settings[row.key] = row.min end
        end
        saveSettings()
      end
    elseif key == "x" or key == "tab" then
      -- B / SELECT: back to menu
      state = "menu"
    end
  end
end

-- ----- update ---------------------------------------------------------------

local function launcherUpdate(dt)
  -- Drive the async boot-time ROM scan before anything else, so the ring
  -- keeps painting between ROM reads.
  if scan.active then
    local ok, err = coroutine.resume(scan.co)
    if not ok then
      scan.active = false
      state = "menu"
      setNotice("Scan error: " .. tostring(err))
      return
    end
    if not scan.active then
      roms = scan.result or {}
      scan.result = nil
      if selectedIdx > #roms then selectedIdx = #roms end
      if selectedIdx < 1 then selectedIdx = 1 end
      state = scan.goTo or "romlist"
      print("[launcher] scanned: " .. #roms .. " ROM(s)")
    end
    return
  end
  if import.active then
    importUpdate()
    return
  end
  if state == "booting" then
    bootPendingUpdate()
    return
  end
  handleTouch()
end

-- ----- top screen (3D) ------------------------------------------------------

local function drawTop()
  -- Pallet Town (HD fan art) as the backdrop, recessed BEHIND the screen
  -- plane so the 3D slider pushes it deep while the logo pops forward -- a
  -- very visible depth gap between the two.  The 400x360 (10:9) PNG is
  -- scaled to fill the 160x144 top layer (aspect already matches).
  if pallet then
    love.graphics._parallax = -0.8
    love.graphics.setColor(1, 1, 1, 1)
    love.graphics.draw(pallet, 0, 0, 0, 160 / palletW, 144 / palletH)
  else
    -- Fallback if the image asset is missing on the SD card.
    love.graphics.setColor(0.13, 0.16, 0.32, 1)
    love.graphics._parallax = -0.8
    love.graphics.rectangle("fill", 0, 0, 160, 144)
  end

  -- Semi-transparent dark overlay on the same depth so the whole backdrop
  -- recedes as one piece and the logo / UI read clearly over the art.
  love.graphics.setColor(0.01, 0.02, 0.08, 0.5)
  love.graphics._parallax = -0.8
  love.graphics.rectangle("fill", 0, 0, 160, 144)

  -- Logo: strong POSITIVE parallax so it POPS OUT in front of the recessed
  -- backdrop (like the game's P_MENU 0.7 / P_FRONT 0.6).  Small disparity
  -- keeps it glasses-free friendly.  Fitted to ~150 game px wide and centered.
  love.graphics._parallax = 0.7
  if logo then
    local scale = math.min(150 / logoW, 36 / logoH)
    local dw = logoW * scale
    local dh = logoH * scale
    love.graphics.setColor(1, 1, 1, 1)
    love.graphics.draw(logo, math.floor((160 - dw) / 2),
                       math.floor((144 - dh) / 2), 0, scale, scale)
  end
end

-- ----- bottom screen (320x240) ---------------------------------------------

-- love.graphics.print is _printC: it renders the citro2d builtin font at
-- scale 0.40, so every glyph is ~12px wide and ~12px tall (the C2D glyph
-- cell is 30px).  Centering math uses this constant, not the 8px a naive
-- monospace guess would assume -- the old one left every centered line ~4px
-- (and long scan/import lines far more) off to the LEFT of the ring.
local FONT_W = 12
local FONT_H = 12

-- Draw `text` horizontally centered on the 320px-wide bottom screen.
local function centerText(text, y)
  love.graphics.print(text, math.floor((320 - #text * FONT_W) / 2), y)
end

-- Keep a line on the 320px bottom screen: at FONT_W px/char it holds 26
-- glyphs, so anything longer (e.g. a long ROM filename) is truncated with an
-- ellipsis instead of overflowing both sides of the centered layout.
local function fitText(text, maxChars)
  maxChars = maxChars or 26
  if #text > maxChars then return text:sub(1, maxChars - 1) .. "..." end
  return text
end

local function drawButton(x, y, w, h, label, id)
  local base = { jogo = { 0.18, 0.30, 0.62 }, mods = { 0.25, 0.25, 0.28 },
                 iniciar = { 0.16, 0.52, 0.26 } }
  local c = base[id] or { 0.3, 0.3, 0.3 }
  -- Pressed state: brighten the button while the finger is on it and for a
  -- short flash after a tap lands, so every touch gives instant feedback.
  local now = love.timer.getTime()
  local down = false
  if love.touch.isDown() then
    local px, py = love.touch.getPosition()
    down = hit(px, py, x, y, w, h)
  end
  if down or (pressFx.id == id and now < pressFx.pressUntil) then
    c = { math.min(1, c[1] + 0.28), math.min(1, c[2] + 0.28),
          math.min(1, c[3] + 0.28) }
  end
  love.graphics.setColor(c[1], c[2], c[3], 1)
  love.graphics.rectangle("fill", x, y, w, h)
  love.graphics.setColor(0.9, 0.95, 1, 1)
  local textW = #label * FONT_W
  love.graphics.print(label, x + (w - textW) / 2, y + (h - FONT_H) / 2)
end

local function drawMenu()
  -- Title area with selected ROM info
  local sel = roms[selectedIdx]
  if sel then
    love.graphics.setColor(0.70, 0.80, 1, 1)
    centerText(fitText(sel.name, 24), 28)
    if sel.version then
      local ready = sel.ready
      local info = GameVersion.info(sel.version)
      love.graphics.setColor(ready and 0.30 or 0.90,
                             ready and 0.80 or 0.50,
                             ready and 0.30 or 0.30, 1)
      centerText(info.displayName .. (ready and " [PRONTO]"
                                        or " [IMPORTAR]"), 46)
    else
      love.graphics.setColor(0.95, 0.5, 0.4, 1)
      centerText("ROM incompativel", 46)
    end
  else
    love.graphics.setColor(0.55, 0.60, 0.70, 1)
    centerText("Nenhuma ROM selecionada", 28)
    centerText("Toque em GAME para escolher", 46)
  end
  -- Buttons
  for _, b in ipairs(MENU_BUTTONS) do
    drawButton(b.x, b.y, b.w, b.h, b.label, b.id)
  end
  -- Physical-input focus: outline the focused button with a thin border
  local fb = MENU_BUTTONS[focusIdx]
  if fb then
    love.graphics.setColor(1, 1, 1, 1)
    love.graphics.rectangle("fill", fb.x - 2, fb.y - 2, fb.w + 4, 2)
    love.graphics.rectangle("fill", fb.x - 2, fb.y + fb.h, fb.w + 4, 2)
    love.graphics.rectangle("fill", fb.x - 2, fb.y, 2, fb.h)
    love.graphics.rectangle("fill", fb.x + fb.w, fb.y, 2, fb.h)
  end
  -- Gear icon (top-right corner, 24x24)
  if gearIcon then
    love.graphics.setColor(1, 1, 1, 1)
    love.graphics.draw(gearIcon, 292, 4, 0, 24 / 96, 24 / 96)
  else
    love.graphics.setColor(0.5, 0.5, 0.6, 1)
    love.graphics.rectangle("fill", 292, 4, 24, 24)
    love.graphics.setColor(1, 1, 1, 1)
    love.graphics.print("*", 299, 6)
  end
end

-- Settings screen (bottom 320x240)
-- Count interactive (non-section) rows for focus mapping
local function settingsInteractiveCount()
  local n = 0
  for _, row in ipairs(SETTINGS_ROWS) do
    if row.key then n = n + 1 end
  end
  return n
end

-- Map 1-based interactive index to SETTINGS_ROWS index
local function settingsRowByInteractive(idx)
  local n = 0
  for i, row in ipairs(SETTINGS_ROWS) do
    if row.key then
      n = n + 1
      if n == idx then return i, row end
    end
  end
  return nil, nil
end

-- Visible rows on screen: header area y=28..210, each row is 20px, section
-- headers are 16px.  Max ~9 rows visible at once.
local SETTINGS_LIST_TOP = 30
local SETTINGS_ROW_H = 20
local SETTINGS_SECTION_H = 16
local SETTINGS_VISIBLE = 9

local function drawSettings()
  love.graphics.setColor(0.10, 0.12, 0.20, 1)
  love.graphics.rectangle("fill", 0, 0, 320, 240)
  -- Title
  love.graphics.setColor(0.80, 0.85, 1, 1)
  centerText("CONFIGURACOES", 8)
  -- Separator
  love.graphics.setColor(0.30, 0.35, 0.50, 1)
  love.graphics.rectangle("fill", 20, 22, 280, 1)

  -- Build visible window around the focused row
  local interactiveIdx = 1
  for ri = 1, settingsIdx - 1 do
    if SETTINGS_ROWS[ri] and SETTINGS_ROWS[ri].key then
      interactiveIdx = interactiveIdx + 1
    end
  end

  -- Calculate scroll offset so focused row is visible
  local visibleRows = 0
  local totalH = 0
  local yOffsets = {}
  for i, row in ipairs(SETTINGS_ROWS) do
    local h = row.section and SETTINGS_SECTION_H or SETTINGS_ROW_H
    yOffsets[i] = totalH
    totalH = totalH + h
  end
  -- Ensure settingsScroll keeps focused row visible
  local focusedY = yOffsets[settingsIdx] or 0
  local focusedH = (SETTINGS_ROWS[settingsIdx] and SETTINGS_ROWS[settingsIdx].section)
                   and SETTINGS_SECTION_H or SETTINGS_ROW_H
  if focusedY < yOffsets[settingsScroll] or settingsScroll > #SETTINGS_ROWS then
    settingsScroll = settingsIdx
  end
  local bottomEdge = (yOffsets[settingsScroll] or 0)
  -- Walk forward from scroll to find how many fit
  local edgeY = 0
  settingsScroll = settingsIdx
  -- Walk backward to find scroll position
  local accH = 0
  for si = settingsIdx, 1, -1 do
    local h = (SETTINGS_ROWS[si] and SETTINGS_ROWS[si].section)
              and SETTINGS_SECTION_H or SETTINGS_ROW_H
    accH = accH + h
    if accH > SETTINGS_VISIBLE * SETTINGS_ROW_H then
      settingsScroll = si + 1
      break
    end
    settingsScroll = si
  end

  -- Draw visible rows
  local drawY = SETTINGS_LIST_TOP
  local drawn = 0
  for i = settingsScroll, #SETTINGS_ROWS do
    local row = SETTINGS_ROWS[i]
    local h = row.section and SETTINGS_SECTION_H or SETTINGS_ROW_H
    if drawY + h > 214 then break end  -- leave room for hint

    local isSelected = (settingsIdx == i)

    if row.section then
      -- Section header
      love.graphics.setColor(0.45, 0.55, 0.80, 1)
      love.graphics.rectangle("fill", 10, drawY, 300, h - 1)
      love.graphics.setColor(1, 1, 1, 1)
      love.graphics.print(row.section, 16, drawY + 2)
    else
      -- Option row
      if isSelected then
        love.graphics.setColor(0.20, 0.25, 0.42, 1)
        love.graphics.rectangle("fill", 10, drawY, 300, h - 2)
      end
      -- Label
      love.graphics.setColor(isSelected and 1 or 0.75,
                             isSelected and 1 or 0.80,
                             isSelected and 1 or 0.90, 1)
      love.graphics.print(row.label, 18, drawY + 4)
      -- Value
      local valStr = ""
      if row.type == "toggle" then
        valStr = settings[row.key] and "ON" or "OFF"
      elseif row.type == "choice" then
        local cur = settings[row.key]
        for _, c in ipairs(row.choices) do
          if c[1] == cur then valStr = c[2]; break end
        end
        if valStr == "" then valStr = tostring(cur) end
      elseif row.type == "number" then
        valStr = tostring(settings[row.key])
      end
      love.graphics.setColor(isSelected and 0.4 or 0.5,
                             isSelected and 0.85 or 0.60,
                             isSelected and 0.5 or 0.55, 1)
      love.graphics.print(valStr, 220, drawY + 4)
    end
    drawY = drawY + h
    drawn = drawn + 1
  end

  -- Scroll indicator arrows
  if settingsScroll > 1 then
    love.graphics.setColor(0.5, 0.6, 0.8, 1)
    love.graphics.print("^", 306, SETTINGS_LIST_TOP)
  end
  if drawY < 214 then
    -- Check if there are more rows below
    local moreBelow = false
    for di = settingsScroll + drawn, #SETTINGS_ROWS do
      if SETTINGS_ROWS[di] then moreBelow = true; break end
    end
    if moreBelow then
      love.graphics.setColor(0.5, 0.6, 0.8, 1)
      love.graphics.print("v", 306, 200)
    end
  end

  -- Hint
  love.graphics.setColor(0.40, 0.45, 0.55, 1)
  centerText("A: alterar  B: voltar", 224)
end

local function drawRomList()
  -- Title bar with centered header
  love.graphics.setColor(0.10, 0.12, 0.20, 1)
  love.graphics.rectangle("fill", 0, 0, 320, 32)
  love.graphics.setColor(0.4, 0.55, 0.9, 1)
  love.graphics.rectangle("fill", 10, 6, 48, 20)
  love.graphics.setColor(1, 1, 1, 1)
  love.graphics.print("BACK", 14, 10)
  love.graphics.setColor(0.85, 0.9, 1, 1)
  centerText("ROMs", 10)

  if #roms == 0 then
    love.graphics.setColor(0.6, 0.65, 0.75, 1)
    centerText("Nenhuma ROM encontrada.", 80)
    love.graphics.setColor(0.45, 0.6, 0.8, 1)
    centerText("Coloque .gb/.gbc em:", 100)
    centerText("sdmc:/.../roms", 116)
    love.graphics.setColor(0.6, 0.65, 0.75, 1)
    centerText("e toque em GAME novamente.", 136)
    return
  end

  -- Ensure selectedIdx is visible in the scroll window
  local visibleCount = 7   -- how many rows fit in the scroll area
  local rowH = 26
  local listTop = 36
  if selectedIdx < romListScroll then
    romListScroll = selectedIdx
  elseif selectedIdx >= romListScroll + visibleCount then
    romListScroll = selectedIdx - visibleCount + 1
  end
  -- Clamp scroll
  if romListScroll < 1 then romListScroll = 1 end
  local maxScroll = math.max(1, #roms - visibleCount + 1)
  if romListScroll > maxScroll then romListScroll = maxScroll end

  -- Draw visible ROM rows
  for vi = 0, visibleCount - 1 do
    local i = romListScroll + vi
    if i > #roms then break end
    local y = listTop + vi * rowH
    local r = roms[i]
    local sel = (i == selectedIdx)
    -- Row background
    love.graphics.setColor(sel and 0.18 or 0.10,
                           sel and 0.22 or 0.12,
                           sel and 0.38 or 0.18, 1)
    love.graphics.rectangle("fill", 10, y, 300, rowH - 2)
    -- Selection highlight bar
    if sel then
      love.graphics.setColor(0.30, 0.45, 0.80, 1)
      love.graphics.rectangle("fill", 10, y, 3, rowH - 2)
    end
    -- ROM name (truncated to fit)
    love.graphics.setColor(1, 1, 1, 1)
    love.graphics.print(fitText(r.name, 22), 20, y + 3)
    -- Version + status
    if r.version then
      local ready = r.ready
      local info = GameVersion.info(r.version)
      love.graphics.setColor(ready and 0.35 or 0.95,
                             ready and 0.85 or 0.55,
                             ready and 0.35 or 0.35, 1)
      love.graphics.print(fitText(info.displayName
        .. (ready and " [OK]" or " [!]"), 22), 20, y + 14)
    else
      love.graphics.setColor(0.95, 0.5, 0.4, 1)
      love.graphics.print("Incompativel", 20, y + 14)
    end
  end

  -- Scroll indicators
  if romListScroll > 1 then
    love.graphics.setColor(0.5, 0.6, 0.8, 1)
    centerText("^", listTop - 4)
  end
  if romListScroll + visibleCount <= #roms then
    love.graphics.setColor(0.5, 0.6, 0.8, 1)
    centerText("v", listTop + visibleCount * rowH)
  end

  -- Counter at bottom
  love.graphics.setColor(0.45, 0.5, 0.6, 1)
  centerText(string.format("%d / %d", selectedIdx, #roms), 226)
end

-- Pokeball as a PNG sprite strip with a gentle continuous sway (the CSS
-- @keyframes shake: tilt + small side shift), looping forever.  No drop-in
-- and no catch darkening.  The 3DS image draw has no rotation, so the strip
-- pre-bakes 13 tilt frames (-24..+24 degrees) and we draw the nearest one
-- through a plain {_x,_y,_w,_h} quad (same shape as love.graphics.newQuad --
-- works in the desktop harness).
local POKEBALL_FRAMES = 13
local POKEBALL_FRAME_PX = 64
local POKEBALL_QUADS = {}   -- filled once the strip loads in M.boot
local pokeball, pokeballW, pokeballH

local function drawPokeball(cx, cy, size)
  if not pokeball or #POKEBALL_QUADS == 0 then return end
  local t = love.timer.getTime()
  local T = 2.2                     -- one full sway cycle (s)
  local p = (t % T) / T             -- 0..1
  -- CSS @keyframes shake over one smooth loop:
  --   0%:0  10%:-20  20%:+20  30%:-20  40%:+20  50%:-12  60%:+12
  --  70%:-4  80%:+4  90%:0  100%:0 (deg)
  local deg = 0
  if p < 0.1 then
    deg = -20 * (p / 0.1)
  elseif p < 0.2 then
    deg = -20 + 40 * ((p - 0.1) / 0.1)
  elseif p < 0.3 then
    deg = 20 - 40 * ((p - 0.2) / 0.1)
  elseif p < 0.4 then
    deg = -20 + 40 * ((p - 0.3) / 0.1)
  elseif p < 0.5 then
    deg = 20 - 32 * ((p - 0.4) / 0.1)
  elseif p < 0.6 then
    deg = -12 + 24 * ((p - 0.5) / 0.1)
  elseif p < 0.7 then
    deg = 12 - 16 * ((p - 0.6) / 0.1)
  elseif p < 0.8 then
    deg = -4 + 8 * ((p - 0.7) / 0.1)
  elseif p < 0.9 then
    deg = 4 - 4 * ((p - 0.8) / 0.1)
  end
  local xOff = -(deg / 24) * 6      -- small side shift with the tilt
  local fi = math.floor((deg + 24) / 4 + 0.5)
  if fi < 0 then fi = 0
  elseif fi > POKEBALL_FRAMES - 1 then fi = POKEBALL_FRAMES - 1 end
  local scale = size / POKEBALL_FRAME_PX
  local x = math.floor(cx - size / 2 + xOff)
  local y = math.floor(cy - size / 2)
  love.graphics.draw(pokeball, POKEBALL_QUADS[fi + 1], x, y, 0, scale, scale)
end

local function drawImport()
  local info = import.version and GameVersion.info(import.version)
  -- Pokeball swaying, game name, import label, a big percentage and the
  -- current stage -- all centered so nothing runs off the 320px screen.
  drawPokeball(160, 84, 64)
  love.graphics.setColor(1, 1, 1, 1)
  centerText(fitText(info and info.displayName or "Game", 20), 128)
  love.graphics.setColor(0.85, 0.9, 1, 1)
  centerText("importing cache...", 142)
  love.graphics.setColor(0.55, 0.8, 1, 1)
  centerText(string.format("%d%%", math.floor((import.progress or 0) * 100)),
             168)
  if import.status and import.status ~= "" then
    love.graphics.setColor(0.9, 0.9, 0.9, 1)
    centerText(fitText(import.status, 26), 194)
  end
end

-- Loading ring + status while the async GAME scan walks the ROMs.  Same ring
-- as the importer, so the launcher button reads as "loading this action"
-- immediately after the tap instead of freezing the screen.
local function drawScanning()
  drawPokeball(160, 96, 64)
  love.graphics.setColor(1, 1, 1, 1)
  centerText("Scanning ROMs...", 150)
  if scan.total > 0 then
    love.graphics.setColor(0.85, 0.9, 1, 1)
    local count = string.format("%d / %d", scan.current, scan.total)
    love.graphics.print(count, math.floor((320 - #count * FONT_W) / 2), 186)
  end
end

local function drawBooting()
  drawPokeball(160, 96, 64)
  love.graphics.setColor(1, 1, 1, 1)
  centerText("Carregando...", 150)
end

local function drawBottom()
  love.graphics.setColor(0.05, 0.06, 0.10, 1)
  love.graphics.rectangle("fill", 0, 0, 320, 240)

  if state == "menu" then
    drawMenu()
  elseif state == "romlist" then
    drawRomList()
  elseif state == "scanning" then
    drawScanning()
  elseif state == "importing" then
    drawImport()
  elseif state == "booting" then
    drawBooting()
  elseif state == "settings" then
    drawSettings()
  end

  if notice ~= "" and love.timer.getTime() < noticeUntil then
    love.graphics.setColor(1, 0.85, 0.30, 1)
    centerText(notice, 218)
  end
end

-- ----- in-game launcher menu (L+R+START) ------------------------------------
-- While a ROM runs, boot() wraps the game's handlers in the functions below.
-- They track L/R held state (the C bridge maps the 3DS shoulders to "l"/"r")
-- and, when START is pressed while both are held, pause the game and open a
-- small launcher menu on the bottom screen: return to the launcher / keep
-- playing.  B or "Continuar" resumes the game untouched.

local GAME_MENU_ITEMS = {
  { label = "Voltar ao launcher", y = 78 },
  { label = "Continuar",          y = 130 },
}

-- (heldL/heldR/gameMenu/gameMenuChoice are forward-declared at the top)

local function gameMenuDraw()
  love.graphics.setColor(0.03, 0.04, 0.10, 0.94)
  love.graphics.rectangle("fill", 0, 0, 320, 240)
  love.graphics.setColor(1, 1, 1, 1)
  centerText("LAUNCHER", 36)
  for i, item in ipairs(GAME_MENU_ITEMS) do
    local sel = (i == gameMenuChoice)
    local c = sel and { 0.16, 0.30, 0.62 } or { 0.13, 0.16, 0.24 }
    love.graphics.setColor(c[1], c[2], c[3], 1)
    love.graphics.rectangle("fill", 20, item.y, 280, 42)
    if sel then
      love.graphics.setColor(1, 1, 1, 1)
      love.graphics.rectangle("fill", 18, item.y - 2, 284, 2)
      love.graphics.rectangle("fill", 18, item.y + 42, 284, 2)
      love.graphics.rectangle("fill", 18, item.y, 2, 42)
      love.graphics.rectangle("fill", 300, item.y, 2, 42)
    end
    love.graphics.setColor(0.9, 0.95, 1, 1)
    love.graphics.print(item.label,
      math.floor((320 - #item.label * FONT_W) / 2), item.y + (42 - FONT_H) / 2)
  end
  love.graphics.setColor(0.55, 0.6, 0.7, 1)
  centerText("L + R + START", 212)
end

local function gameMenuTouch()
  if not love.touch.getPressed() then return end
  local px, py = love.touch.getPosition()
  for i, item in ipairs(GAME_MENU_ITEMS) do
    if hit(px, py, 20, item.y, 280, 42) then
      if i == 1 then exitToLauncher() else closeGameMenu() end
      return
    end
  end
end

openGameMenu = function()
  gameMenu = true
  gameMenuChoice = 2              -- default "Continuar" (safe)
  love.graphics._drawBottom = gameMenuDraw
end

closeGameMenu = function()
  gameMenu = false
  love.graphics._drawBottom = nil
end

exitToLauncher = function()
  gameMenu = false
  -- Stop audio so the old game's music doesn't keep playing.
  if love.audio and love.audio.stop then
    pcall(love.audio.stop)
  end
  print("[exit] returning to launcher")
  -- Drop the running game and swap back to the launcher's own handlers.
  love.keypressed = launcherKeypressed
  love.keyreleased = function() end
  love.gamepadpressed = function() end
  love.update = launcherUpdate
  love.draw = drawTop
  love.graphics._drawBottom = drawBottom
  -- Reset graphics state so the old game's canvas/shader/scissor don't leak.
  if love.graphics.setCanvas then pcall(love.graphics.setCanvas) end
  if love.graphics.setShader then pcall(love.graphics.setShader, nil) end
  if love.graphics.setScissor then pcall(love.graphics.setScissor) end
  -- Unmount the old version's PhysFS cache so the next boot starts clean.
  -- Without this, the old version's generated files remain visible to
  -- love.filesystem / require and the second boot gets stale cached data.
  pcall(function()
    local ver = require("src.core.GameVersion").get()
    if ver then CacheFs.unmountVersion(ver) end
  end)
  -- Drop generated data modules from the require cache so Data:load()
  -- re-reads from the new version's files on the next boot.
  pcall(function() require("src.core.Data"):unloadGenerated() end)
  -- Drop the old Game so its (megabyte-scale) data becomes garbage.
  _G.Game = nil
  collectgarbage("collect")
  -- Refresh ready flags: a just-completed import marks its cache as ready
  -- so the menu shows [READY] instead of [IMPORTAR].
  for _, r in ipairs(roms) do
    if r.version then r.ready = versionReady(r.version) end
  end
  state = "menu"
  selectedIdx = 1
  focusIdx = 1
  notice = ""
  print("[launcher] returned from game to launcher")
end

gameUpdate = function(dt)
  if gameMenu then
    gameMenuTouch()
    return    -- paused: the sim does not step while the menu is open
  end
  if savedUpdate then savedUpdate(dt) end
end

gameKeypressed = function(key, scancode, isrepeat)
  if key == "l" then heldL = true
  elseif key == "r" then heldR = true
  end
  if gameMenu then
    if key == "up" then
      gameMenuChoice = gameMenuChoice - 1
      if gameMenuChoice < 1 then gameMenuChoice = #GAME_MENU_ITEMS end
    elseif key == "down" then
      gameMenuChoice = gameMenuChoice + 1
      if gameMenuChoice > #GAME_MENU_ITEMS then gameMenuChoice = 1 end
    elseif (key == "z" or key == "kpenter") and not isrepeat then
      if gameMenuChoice == 1 then exitToLauncher() else closeGameMenu() end
    elseif key == "x" then
      closeGameMenu()
    end
    return
  end
  -- L+R+START: open the launcher menu (swallow the START so the game's own
  -- menu does not open too).
  if key == "kpenter" and heldL and heldR and not isrepeat then
    openGameMenu()
    return
  end
  if savedKeypressed then savedKeypressed(key, scancode, isrepeat) end
end

gameKeyreleased = function(key)
  if key == "l" then heldL = false
  elseif key == "r" then heldR = false
  end
  if gameMenu then return end
  if savedKeyreleased then savedKeyreleased(key) end
end

-- ----- boot entry -----------------------------------------------------------

function M.boot(bootFn)
  bootGameFn = bootFn
  savedUpdate = love.update
  savedDraw = love.draw

  local okLogo = pcall(function()
    logo = love.graphics.newImage("assets/logo/logo.png")
    logoW, logoH = logo:getDimensions()
  end)
  if not okLogo then logo = nil end

  local okPallet = pcall(function()
    pallet = love.graphics.newImage("assets/launcher/pallet.png")
    palletW, palletH = pallet:getDimensions()
  end)
  if not okPallet then pallet = nil end

  local okPokeball = pcall(function()
    pokeball = love.graphics.newImage("assets/launcher/pokeball.png")
    pokeballW, pokeballH = pokeball:getDimensions()
    POKEBALL_QUADS = {}
    for f = 0, POKEBALL_FRAMES - 1 do
      POKEBALL_QUADS[f + 1] = { _x = f * POKEBALL_FRAME_PX, _y = 0,
                                _w = POKEBALL_FRAME_PX, _h = POKEBALL_FRAME_PX }
    end
  end)
  if not okPokeball then pokeball = nil end

  local okGear = pcall(function()
    gearIcon = love.graphics.newImage("assets/launcher/gear.png")
  end)
  if not okGear then gearIcon = nil end

  loadSettings()

  print("[launcher] assets: logo=" .. (logo and "ok" or "missing")
        .. " pallet=" .. (pallet and "ok" or "missing")
        .. " pokeball=" .. (pokeball and "ok" or "missing"))

  -- While the launcher is on screen, the game's input handlers would hit a
  -- nil Game and spam errors; swap in the launcher's D-pad/A/B handler (and
  -- no-op the rest) and restore the game's real ones on boot.  The game's
  -- handlers are captured ONCE: M.boot can be re-entered when the player
  -- returns from an in-game ROM to the launcher, and re-saving there would
  -- chain the L+R+START wrappers (installed by boot) onto themselves.
  if not pristineCaptured then
    pristineCaptured = true
    pristineUpdate = love.update       -- game's love.update from main.lua
    pristineDraw = love.draw           -- game's love.draw from main.lua
    savedKeypressed = love.keypressed
    savedKeyreleased = love.keyreleased
    savedGamepadpressed = love.gamepadpressed
  end
  love.keypressed = launcherKeypressed
  love.keyreleased = function() end
  love.gamepadpressed = function() end

  -- Scan the ROM folder asynchronously: the launcher (with its loading ring)
  -- appears immediately and lands on the menu when the scan finishes.
  startScan("menu")
  selectedIdx = 1

  love.update = launcherUpdate
  love.draw = drawTop
  love.graphics._drawBottom = drawBottom
  print("3DS launcher ready: " .. #roms .. " ROM(s), top 3D + bottom UI")
end

-- Small introspection helper (used by the desktop test harness; harmless in
-- the running launcher).
function M.debug()
  return { state = state, roms = roms, selectedIdx = selectedIdx,
           focusIdx = focusIdx, scanActive = scan.active,
           importActive = import.active, importProgress = import.progress }
end

return M
