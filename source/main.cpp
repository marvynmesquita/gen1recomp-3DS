#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

int luaopen_bit(lua_State *L);
}
#endif

#include <stdarg.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

static u64 g_last_slow_frame_log_ms = 0;

// Per-frame draw diagnostics collected by the graphics binding
// (love_graphics.cpp).  Reset at the top of every frame, logged by the
// slow-frame line so we can see exactly how many C2D_DrawImageAt calls the
// frame issued and how much of the draw time they took.
extern unsigned g_dbg_batch_calls;
extern unsigned g_dbg_batch_entries;
extern unsigned g_dbg_single_calls;
extern u64 g_dbg_batch_ms;
extern u64 g_dbg_draw_cpp_ms;
extern unsigned g_dbg_tex_switches;
extern C3D_Tex* g_last_tex;

// Stereoscopic parallax per eye, set by the main loop before each love.draw
// (+slider for the left eye, -slider for the right, 0 when the 3D slider is
// down).  Consumed by love_graphics.cpp's draw paths.
extern float g_eyeParallax;

// Graphics bindings for Canvas support (love_graphics.cpp)
extern "C" {
void graphics_setScreenTarget(C3D_RenderTarget* target);
void graphics_setScreenScale(float sx, float sy);
}

// Asynchronous RAM log buffer: SD card file writes (fopen/fclose) on the 3DS
// take 150-200ms of synchronous disk I/O, which turned log writes during
// gameplay into massive frame hitches. sys_log now appends instantly to a RAM
// buffer and flushes to sdmc:/3ds/gen1recomp3ds/log.txt in a background thread.
static char g_log_buffer_1[32768];
static char g_log_buffer_2[32768];
static char* g_log_current = g_log_buffer_1;
static size_t g_log_current_len = 0;

static LightEvent g_log_event;
static LightLock g_log_lock;
static bool g_log_quit = false;
static Thread g_log_thread = nullptr;

static void log_thread_func(void* arg) {
    // Ensure the log directory exists.  This avoids a NULL devoptab crash
    // on first launch when the launcher's main() mkdir() pass hasn't run yet
    // (e.g. a process restart or after a failed import left /3ds missing).
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/gen1recomp3ds", 0777);

    // Persistent log file handle.  We keep it open to avoid the race where
    // CacheFs.unmountVersion() remounts the SD card between open() and write().
    // On error (e.g. device unmounted), we close the handle and retry on next flush.
    int log_fd = -1;
    bool log_fd_valid = false;

    while (true) {
        LightEvent_Wait(&g_log_event);
        
        char* buf_to_write = nullptr;
        size_t len_to_write = 0;
        
        LightLock_Lock(&g_log_lock);
        if (g_log_current_len > 0) {
            buf_to_write = g_log_current;
            len_to_write = g_log_current_len;
            g_log_current = (g_log_current == g_log_buffer_1) ? g_log_buffer_2 : g_log_buffer_1;
            g_log_current_len = 0;
        }
        LightEvent_Clear(&g_log_event);
        LightLock_Unlock(&g_log_lock);
        
        if (buf_to_write && len_to_write > 0) {
            // Ensure we have a valid log file handle
            if (!log_fd_valid) {
                log_fd = open("sdmc:/3ds/gen1recomp3ds/log.txt", O_WRONLY | O_APPEND | O_CREAT, 0644);
                log_fd_valid = (log_fd >= 0);
            }
            if (log_fd_valid) {
                ssize_t written = write(log_fd, buf_to_write, len_to_write);
                if (written < 0 || (size_t)written != len_to_write) {
                    // Write failed (e.g. device unmounted). Close stale handle and
                    // drop this batch; we'll retry opening on the next flush.
                    close(log_fd);
                    log_fd = -1;
                    log_fd_valid = false;
                }
            }
        }
        
        if (g_log_quit) {
            break;
        }
    }
    if (log_fd_valid) close(log_fd);
}

static void flush_log_buffer() {
    LightEvent_Signal(&g_log_event);
}

extern "C" void sys_log(const char* format, ...) {
    u64 now_ms = osGetTime();
    char line[512];
    int header_len = snprintf(line, sizeof(line), "[%lu] ", (unsigned long)now_ms);
    if (header_len < 0 || header_len >= (int)sizeof(line)) return;

    va_list args;
    va_start(args, format);
    int msg_len = vsnprintf(line + header_len, sizeof(line) - header_len - 2, format, args);
    va_end(args);

    if (msg_len < 0) return;
    size_t total_len = header_len + msg_len;
    line[total_len++] = '\n';
    line[total_len] = '\0';

    LightLock_Lock(&g_log_lock);
    if (g_log_current_len + total_len >= 32768) {
        LightEvent_Signal(&g_log_event); // Flush when full
    } else {
        memcpy(g_log_current + g_log_current_len, line, total_len);
        g_log_current_len += total_len;
        LightEvent_Signal(&g_log_event); // Trigger background flush immediately!
    }
    LightLock_Unlock(&g_log_lock);
}

static int l_print(lua_State* L) {
    int nargs = lua_gettop(L);
    for (int i = 1; i <= nargs; i++) {
        const char* str = lua_tostring(L, i);
        if (str) {
            sys_log("%s", str);
        }
    }
    return 0;
}

// Clean "Loading..." splash shown on both screens while Lua boots.  This
// replaced the old bottom-screen console (consoleInit(GFX_BOTTOM)), whose
// libctru framebuffer writes fought with citro3d's own bottom render target --
// that is what caused the bottom-screen glitch and the boot log printed under
// the launcher UI.  Debug output still lands in sdmc:/3ds/gen1recomp3ds/log.txt
// via sys_log(), so no diagnostic information is lost.
static void show_loading_splash(C3D_RenderTarget* top, C3D_RenderTarget* bottom) {
    C2D_TextBuf buf = C2D_TextBufNew(512);
    if (!buf) return;

    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);

    // Top: same background colour as the launcher's first frame, so the
    // transition into the menu is seamless (no flash / garbage between boot
    // and the first Lua frame).
    C2D_TargetClear(top, C2D_Color32(0x21, 0x29, 0x52, 0xFF));
    C2D_SceneBegin(top);

    // Bottom: dark launcher background + centred "Loading...".
    C2D_TargetClear(bottom, C2D_Color32(0x10, 0x10, 0x18, 0xFF));
    C2D_SceneBegin(bottom);
    C2D_Text t;
    C2D_TextParse(&t, buf, "Loading...");
    C2D_TextOptimize(&t);
    float w = 0.0f, h = 0.0f;
    const float scale = 0.8f;
    C2D_TextGetDimensions(&t, 1.0f, 1.0f, &w, &h);
    C2D_DrawText(&t, C2D_WithColor,
                 (320.0f - w * scale) / 2.0f,
                 (240.0f - h * scale) / 2.0f,
                 0.5f, scale, scale,
                 C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF));

    C3D_FrameEnd(0);
    C2D_TextBufDelete(buf);
}

// Forward declarations of our love modules
extern "C" {
    int luaopen_love_graphics(lua_State* L);
    int luaopen_love_keyboard(lua_State* L);
    int luaopen_love_event(lua_State* L);
    int luaopen_love_system(lua_State* L);
    int luaopen_love_timer(lua_State* L);
    int luaopen_love_filesystem(lua_State* L);
    int luaopen_love_sound(lua_State* L);
    int luaopen_love_audio(lua_State* L);
    int luaopen_love_thread(lua_State* L);
    int luaopen_love_synth(lua_State* L);
    int luaopen_love_image(lua_State* L);
}

extern "C" {
void love_input_poll(lua_State* L, int msgh);
int love_input_want_quit(void);
int love_input_touch_get_position(lua_State* L);
int love_input_touch_is_down(lua_State* L);
int love_input_touch_just_pressed(lua_State* L);
}

// ---- SHA-1 (love.data.hash("sha1", data)) --------------------------------
// Small self-contained implementation so the launcher can identify a ROM by
// its SHA-1 on the 3DS (the engine's GameVersion.forSha1 keys on it).  The
// stubbed love.data.hash used to return '' -- useless for ROM detection.

static void sha1_compute(const uint8_t* data, size_t len, uint8_t out[20]) {
    uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE,
             h3 = 0x10325476, h4 = 0xC3D2E1F0;
    uint64_t bitlen = (uint64_t)len * 8;

    size_t mod = len % 64;
    size_t pad = (mod < 56) ? (56 - mod) : (120 - mod);
    size_t total = len + pad + 8;
    uint8_t* msg = (uint8_t*)malloc(total ? total : 1);
    memcpy(msg, data, len);
    msg[len] = 0x80;
    memset(msg + len + 1, 0, pad - 1);
    for (int i = 0; i < 8; i++)
        msg[total - 8 + i] = (uint8_t)(bitlen >> (56 - 8 * i));

    for (size_t off = 0; off < total; off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) {
            w[i] = ((uint32_t)msg[off + i * 4] << 24)
                 | ((uint32_t)msg[off + i * 4 + 1] << 16)
                 | ((uint32_t)msg[off + i * 4 + 2] << 8)
                 | ((uint32_t)msg[off + i * 4 + 3]);
        }
        for (int i = 16; i < 80; i++) {
            uint32_t v = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
            w[i] = (v << 1) | (v >> 31);
        }
        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | ((~b) & d); k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d;            k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
            else             { f = b ^ c ^ d;            k = 0xCA62C1D6u; }
            uint32_t temp = ((a << 5) | (a >> 27)) + f + e + k + w[i];
            e = d; d = c;
            c = (b << 30) | (b >> 2);
            b = a;
            a = temp;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }
    free(msg);

    uint32_t hs[5] = { h0, h1, h2, h3, h4 };
    for (int i = 0; i < 5; i++) {
        out[i * 4]     = (uint8_t)(hs[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(hs[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(hs[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(hs[i]);
    }
}

static int l_data_hash(lua_State* L) {
    const char* algo = luaL_checkstring(L, 1);
    size_t len;
    const char* data = luaL_checklstring(L, 2, &len);
    if (strcmp(algo, "sha1") != 0) {
        lua_pushstring(L, "");
        return 1;
    }
    uint8_t digest[20];
    sha1_compute((const uint8_t*)data, len, digest);
    lua_pushlstring(L, (const char*)digest, 20);
    return 1;
}

static int l_data_encode(lua_State* L) {
    luaL_checkstring(L, 1); // form ("string")
    const char* format = luaL_checkstring(L, 2);
    size_t len;
    const char* data = luaL_checklstring(L, 3, &len);
    if (strcmp(format, "hex") == 0) {
        static const char hexdigits[] = "0123456789abcdef";
        char* out = (char*)malloc(len * 2 + 1);
        for (size_t i = 0; i < len; i++) {
            out[i * 2]     = hexdigits[(data[i] >> 4) & 0xF];
            out[i * 2 + 1] = hexdigits[data[i] & 0xF];
        }
        lua_pushlstring(L, out, len * 2);
        free(out);
        return 1;
    }
    lua_pushstring(L, "");
    return 1;
}

// ---- SD directory listing (love.filesystem.getDirectoryItems) -------------

static int l_listdir(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    char fullpath[512];
    if (strncmp(path, "sdmc:", 5) == 0 || strncmp(path, "romfs:", 6) == 0) {
        snprintf(fullpath, sizeof(fullpath), "%s", path);
    } else {
        snprintf(fullpath, sizeof(fullpath), "sdmc:/3ds/gen1recomp3ds/%s", path);
    }
    DIR* dir = opendir(fullpath);
    lua_newtable(L);
    if (!dir) return 1;
    int n = 0;
    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        n++;
        lua_pushstring(L, ent->d_name);
        lua_rawseti(L, -2, n);
    }
    closedir(dir);
    return 1;
}

// ---- SD path probe (love.filesystem.getInfo) --------------------------------
// Returns { type = "file", size = n } for files, { type = "directory" } for
// directories, or nil when nothing exists at the path.  The old Lua-only
// getInfo used io.open which can never open a directory, so the mod loader's
// _discover() (which checks fs.getInfo("mods/<id>").type == "directory")
// never found mod folders on the SD card.
static int l_getinfo(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    char fullpath[512];
    if (strncmp(path, "sdmc:", 5) == 0 || strncmp(path, "romfs:", 6) == 0) {
        snprintf(fullpath, sizeof(fullpath), "%s", path);
    } else {
        snprintf(fullpath, sizeof(fullpath), "sdmc:/3ds/gen1recomp3ds/%s", path);
    }
    struct stat st;
    if (stat(fullpath, &st) != 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_newtable(L);
    if (S_ISDIR(st.st_mode)) {
        lua_pushstring(L, "directory");
        lua_setfield(L, -2, "type");
    } else {
        lua_pushstring(L, "file");
        lua_setfield(L, -2, "type");
        lua_pushinteger(L, (lua_Integer)st.st_size);
        lua_setfield(L, -2, "size");
    }
    return 1;
}

static int l_timer_getTime(lua_State* L) {
    // High-resolution timer: svcGetSystemTick() runs at ~268MHz, giving
    // sub-microsecond resolution (osGetTime() is 1ms-granular, which made
    // DrawProf's sub-ms phase readings pure quantization noise).
    lua_pushnumber(L, (double)svcGetSystemTick() / 268123480.0);
    return 1;
}

// Battle draw section breakdown (pics/huds/anim/text ms), published as Lua
// globals by BattleState.noteBattleDraw on every battle frame (both the
// classic and wide layouts).  Reads them here so the "Slow frame!" line --
// the one log entry we know reliably reaches disk -- carries the breakdown.
// Prints "12.3" or "-" when no battle frame has run yet.
static void lua_num_or_dash(lua_State* L, const char* name, char* out, size_t n) {
    lua_getglobal(L, name);
    if (lua_isnumber(L, -1)) {
        snprintf(out, n, "%.1f", (double)lua_tonumber(L, -1));
    } else {
        snprintf(out, n, "-");
    }
    lua_pop(L, 1);
}

// Reads a Lua global string (or "-" when it's not a string).  Used for the
// _dbg_bt_path battle-branch marker.
static void lua_str_or_dash(lua_State* L, const char* name, char* out, size_t n) {
    lua_getglobal(L, name);
    const char* s = lua_tostring(L, -1);
    if (s) {
        snprintf(out, n, "%s", s);
    } else {
        snprintf(out, n, "-");
    }
    lua_pop(L, 1);
}

int main(int argc, char* argv[]) {
    // Initialize services
    romfsInit();
    gfxInitDefault();
    // Enable the top screen's stereoscopic (glasses-free) output so both the
    // left and right eye framebuffers exist; the render loop draws the scene
    // once per eye when the 3D slider is up.  Must run before creating the
    // GFX_RIGHT render target below.
    gfxSet3D(true);
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();

    // Let the audio worker use the second core.  The app's threads run on core 0
    // by default; core 1 is shared with the system and APT_SetAppCpuTimeLimit
    // hands a slice of it to the app.  Without this the synth worker (created
    // later from Lua) is pinned to core 0, the same core the render loop runs
    // on continuously, so it is starved and can never sustain realtime audio.
    //
    // 30% of one core was marginal for the Lua chip synth on Old 3DS hardware:
    // the worker could not quite sustain realtime, so the deep playback queue
    // periodically drained and the music stuttered (especially noticeable on
    // dense overworld maps).  60% gives the synth a real-time headroom while
    // still leaving the system its share of core 1.
    APT_SetAppCpuTimeLimit(60);

    // Create base directories on the SD card
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/gen1recomp3ds", 0777);
    mkdir("sdmc:/3ds/gen1recomp3ds/saves", 0777);
    mkdir("sdmc:/3ds/gen1recomp3ds/states", 0777);
    mkdir("sdmc:/3ds/gen1recomp3ds/cache", 0777);
    mkdir("sdmc:/3ds/gen1recomp3ds/cache/assets", 0777);
    mkdir("sdmc:/3ds/gen1recomp3ds/cache/assets/generated", 0777);
    // Launcher folders: user drops ROMs in roms/ (mods/ is for later).
    mkdir("sdmc:/3ds/gen1recomp3ds/roms", 0777);
    mkdir("sdmc:/3ds/gen1recomp3ds/mods", 0777);


    Result res = ndspInit();
    sys_log("ndspInit() result: %08X", (unsigned int)res);
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);

    // Create screens.  top is the mono / left-eye target; topRight is the
    // right-eye target used only when the 3D slider is up (gfxSet3D above).
    C3D_RenderTarget* top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    C3D_RenderTarget* topRight = C2D_CreateScreenTarget(GFX_TOP, GFX_RIGHT);
    // Bottom screen: the launcher renders its UI here via
    // love.graphics._drawBottom; during gameplay it stays black (the hook is
    // removed) until the two-screen mod is added.
    C3D_RenderTarget* bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    // Clean "Carregando..." splash while Lua boots (replaces the old bottom
    // console, which glitched against C2D and leaked boot logs over the
    // launcher UI).
    show_loading_splash(top, bottom);

    // Initialize Lua
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);

    // Provide _mkdir to Lua (recursive: creates every parent directory so
    // CacheFs.write can land deep paths like blue/assets/generated/... in
    // one shot on first import).
    lua_pushcfunction(L, [](lua_State* L2) -> int {
        const char* path = luaL_checkstring(L2, 1);
        char fullpath[256];
        snprintf(fullpath, sizeof(fullpath), "sdmc:/3ds/gen1recomp3ds/%s", path);
        for (char* p = fullpath; *p; p++) {
            if (*p == '/') {
                char save = *(p + 1);
                *(p + 1) = '\0';
                mkdir(fullpath, 0777);
                *(p + 1) = save;
            }
        }
        mkdir(fullpath, 0777);
        lua_pushboolean(L2, true);
        return 1;
    });
    lua_setglobal(L, "_mkdir");

    // Directory listing (SD card) for the launcher's ROM picker.
    lua_pushcfunction(L, l_listdir);
    lua_setglobal(L, "_listdir");
    // Path probe for getInfo: detects both files and directories.
    lua_pushcfunction(L, l_getinfo);
    lua_setglobal(L, "_getinfo");

    // Setup global "love" table
    lua_newtable(L);
    lua_setglobal(L, "love");

    // Register LuaBitOp in package.preload
    lua_getglobal(L, "package");
    lua_getfield(L, -1, "preload");
    lua_pushcfunction(L, luaopen_bit);
    lua_setfield(L, -2, "bit");
    lua_pop(L, 2);

    // Overwrite print
    lua_pushcfunction(L, l_print);
    lua_setglobal(L, "print");

    // Load our custom love modules
    luaopen_love_graphics(L);
    lua_pop(L, 1);
    luaopen_love_system(L);
    lua_pop(L, 1);
    luaopen_love_sound(L);
    lua_pop(L, 1);
    luaopen_love_audio(L);
    lua_pop(L, 1);
    luaopen_love_thread(L);
    luaopen_love_synth(L);
    // Real ImageData for the runtime ROM importer (love.image._newImageData).
    // The stub_script below deliberately keeps love.image.newImageData false
    // so the engine's slow palette paths stay gated off on the 3DS.
    luaopen_love_image(L);
    lua_pop(L, 1);

    // Setup timer
    lua_getglobal(L, "love");
    lua_newtable(L);
    lua_pushcfunction(L, l_timer_getTime);
    lua_setfield(L, -2, "getTime");
    lua_setfield(L, -2, "timer");
    lua_pop(L, 1);

    // Stub other modules in Lua
    const char* stub_script = R"LUA_STUB(
local stubs = {'audio', 'data', 'filesystem', 'image', 'joystick', 'keyboard', 'math', 'mouse', 'sound', 'touch', 'window', 'event', 'graphics', 'system'}
for _, name in ipairs(stubs) do
  love[name] = love[name] or {}
  -- Preserve any existing __newindex metamethod (e.g. love.graphics's
  -- parallax intercept that updates g_parallaxLayer via C++).  Only the
  -- __index is replaced with the no-op fallback for missing methods.
  local old_mt = getmetatable(love[name])
  setmetatable(love[name], {__index = function(t, k)
    if k == 'getFont' then return function() return love.graphics.newFont() end end
    return function() end
  end, __newindex = old_mt and rawget(old_mt, '__newindex')})
end

love.math = love.math or {}
love.math.random = math.random
love.image.newImageData = false -- trigger headless fallbacks

-- Filesystem stubs
-- _activeCachePrefix (set by the launcher before booting a game, e.g. "blue/")
-- makes SD reads try the version-prefixed cache first, so a runtime-imported
-- cache on SD overlays the romfs root cache (Red).
love.filesystem.getInfo = function(path)
  local prefix = _activeCachePrefix or ""
  if prefix ~= "" then
        local info = _getinfo(prefix .. path)
    if info then return info end
  end
    local info = _getinfo(path)
    if info then return info end
    if path:sub(1, 6) ~= 'romfs:' and path:sub(1, 5) ~= 'sdmc:' then
        return _getinfo('romfs:/' .. path)
    end
    return nil
end
love.filesystem.getRealDirectory = function(path) return 'sdmc:/3ds/gen1recomp3ds' end
love.filesystem.getUserDirectory = function() return 'sdmc:/3ds/gen1recomp3ds' end
love.filesystem.getSaveDirectory = function() return 'sdmc:/3ds/gen1recomp3ds' end
love.filesystem.getAppdataDirectory = function() return 'sdmc:/3ds/gen1recomp3ds' end
love.filesystem.read = function(path)
  local prefix = _activeCachePrefix or ""
  if prefix ~= "" then
    local f = io.open('sdmc:/3ds/gen1recomp3ds/' .. prefix .. path, 'rb')
    if f then
      local d = f:read('*a')
      f:close()
      return d
    end
  end
  local f = io.open('sdmc:/3ds/gen1recomp3ds/' .. path, 'rb')
  if not f then f = io.open('romfs:/' .. path, 'rb') end
  if not f then return nil, 'file not found' end
  local d = f:read('*a')
  f:close()
  return d
end
love.filesystem.getDirectoryItems = function(path)
  return _listdir(path or "")
end
love.filesystem.load = function(path)
  local prefix = _activeCachePrefix or ""
  local function try(full)
    local f = io.open(full, 'rb')
    if not f then return nil end
    local chunk = assert(loadstring(f:read('*a')))
    f:close()
    return chunk
  end
  if prefix ~= "" then
    local c = try('sdmc:/3ds/gen1recomp3ds/' .. prefix .. path)
    if c then return c end
  end
  local c = try('sdmc:/3ds/gen1recomp3ds/' .. path)
  if c then return c end
  return loadfile('romfs:/' .. path)
end
love.filesystem.write = function(path, data) 
  local f = io.open('sdmc:/3ds/gen1recomp3ds/' .. path, 'wb')
  if not f then return false, 'could not open file' end
  local ok, werr = f:write(data)
  f:close()
  if not ok then return false, werr or 'write failed' end
  return true 
end
love.filesystem.createDirectory = function(path)
  -- Implemented via C++ bind below
  return _mkdir(path)
end
love.filesystem.remove = function(path)
  return os.remove('sdmc:/3ds/gen1recomp3ds/' .. path)
end

-- Graphics stubs
-- The 3DS top screen is 400x240. The game canvas is 160x144.
-- We report the GAME dimensions (160x144) to Lua so all layout code works correctly,
-- but when drawing we will scale up in C++.
love.graphics.getWidth = function() return 160 end
love.graphics.getHeight = function() return 144 end
love.graphics.getDimensions = function() return 160, 144 end
love.graphics.getPixelDimensions = function() return 160, 144 end
love.graphics.getDPIScale = function() return 1 end
love.graphics.setDefaultFilter = function() end
love.graphics.isActive = function() return true end
love.graphics.origin = function() end
love.graphics.getBackgroundColor = function() return 1, 1, 1, 1 end
love.graphics.present = function() end
-- setColor and rectangle are implemented in C++ (love_graphics.cpp)
love.graphics.push = function() end
love.graphics.pop = function() end
love.graphics.scale = function() end
love.graphics.translate = function() end
love.graphics.print = love.graphics._printC
love.graphics.printf = function(text, x, y, limit, align)
  return love.graphics._printC(text, x, y)
end
love.graphics.setFont = function() end
-- setScissor(x,y,w,h) stays a no-op for direct-frame rendering, but a
-- NO-ARGUMENT setScissor() must still clear any clip that
-- love.graphics.intersectScissor applied (intersectScissor is real and
-- calls C3D_SetScissor).  If the clear is a no-op too, the clip leaks
-- into the rest of the frame and blanks the HUD / text box (the
-- move-select white-rectangle glitch).
local _realSetScissor = love.graphics.setScissor
love.graphics.setScissor = function(...)
  if select("#", ...) == 0 then _realSetScissor() end
end
-- draw and newImage are implemented in C++ (love_graphics.cpp)

-- Object factories
love.graphics.newQuad = function(x, y, w, h, sw, sh)
  return {
    _x = x, _y = y, _w = w, _h = h,
    getViewport = function(self) return self._x, self._y, self._w, self._h end,
    setViewport = function(self, nx, ny, nw, nh) self._x = nx; self._y = ny; self._w = nw; self._h = nh end,
    typeOf = function(self, t) return t == 'Quad' end
  }
end
-- Canvas is now implemented in C++ (love_graphics.cpp)

-- Shader stubs: 3DS doesn't support GLSL shaders (would need pre-compiled .shbin).
-- We stub them so PaletteFX doesn't crash. Color palette won't apply but game runs.
-- NOTE: returns FALSE, not a stub object.  The whole engine gates expensive
-- zone-scan/merge/shader work on PaletteFX.shader() being truthy (pcall'd
-- newShader -> ok and sh or false -> shader() or nil).  A truthy stub object
-- silently turned those gates ON on the 3DS, so every frame ran the SGB zone
-- scan + endFrame zone merge + per-zone scissor/setShader/sendColors blits
-- and AnimPlayer took its slow color-sampling path.  Returning false (which
-- flows through pcall as a clean value, not an error) makes every one of
-- those gates resolve to the intended unshaded 3DS fast path.
love.graphics.newShader = function(code)
  return false
end
love.graphics._activeShader = nil
love.graphics.setShader = function(sh) love.graphics._activeShader = sh end
love.graphics.getShader = function() return love.graphics._activeShader end

love.graphics.newFont = function()
  return {
    getHeight = function() return 12 end,
    getWidth = function() return 10 end,
    getAscent = function() return 10 end,
    getDescent = function() return 2 end,
    getBaseline = function() return 10 end,
    getFilter = function() return 'nearest', 'nearest' end,
    setFilter = function() end,
    typeOf = function(self, t) return t == 'Font' end
  }
end

-- General stubs
love.timer.getDelta = function() return 1.0/60.0 end
love.timer.sleep = function(n) end -- properly stubbed in C++ later or replaced
love.joystick.getJoystickCount = function() return 0 end
love.joystick.getJoysticks = function() return {} end
love.mouse.getPosition = function() return 0, 0 end
love.mouse.isCursorSupported = function() return false end
love.keyboard.isDown = function() return false end
love.data.hash = function() return '' end
love.data.encode = function() return '00000000' end
love.window.setMode = function() return true end
love.window.getSafeArea = function() return 0, 0, 400, 240 end
love.window.getDisplayCount = function() return 1 end
love.window.getFullscreenModes = function() return {} end
love.window.isOpen = function() return true end

-- SD comes before romfs so a runtime-imported cache can serve generated data
-- via require (the launcher prepends the version-prefixed SD dir on boot).
package.path = 'sdmc:/3ds/gen1recomp3ds/?.lua;sdmc:/3ds/gen1recomp3ds/?/init.lua;romfs:/?.lua;romfs:/?/init.lua;' .. package.path
)LUA_STUB";
    if (luaL_dofile(L, "romfs:/conf.lua") != 0) {
        printf("Error loading conf.lua: %s\n", lua_tostring(L, -1));
        sys_log("Error loading conf.lua: %s", lua_tostring(L, -1));
        printf("\x1b[31mPress START to exit.\x1b[0m\n");
        lua_pop(L, 1);
    }
    if (luaL_dostring(L, stub_script) != 0) {
        printf("Error stubbing: %s\n", lua_tostring(L, -1));
        sys_log("Error stubbing: %s", lua_tostring(L, -1));
        printf("\x1b[31mPress START to exit.\x1b[0m\n");
        lua_pop(L, 1);
    }

    // Replace the stubbed love.keyboard / love.event with real implementations
    // (must run after stub_script so the stubs don't overwrite them).
    luaopen_love_keyboard(L);
    lua_pop(L, 1);
    luaopen_love_event(L);
    lua_pop(L, 1);

    // Real SHA-1 + hex encode for ROM identification (GameVersion.forSha1).
    lua_getglobal(L, "love");
    lua_getfield(L, -1, "data");
    lua_pushcfunction(L, l_data_hash);
    lua_setfield(L, -2, "hash");
    lua_pushcfunction(L, l_data_encode);
    lua_setfield(L, -2, "encode");
    lua_pop(L, 2);

    // Real touch input for the bottom-screen launcher buttons.
    lua_getglobal(L, "love");
    lua_newtable(L);
    lua_pushcfunction(L, love_input_touch_get_position);
    lua_setfield(L, -2, "getPosition");
    lua_pushcfunction(L, love_input_touch_is_down);
    lua_setfield(L, -2, "isDown");
    lua_pushcfunction(L, love_input_touch_just_pressed);
    lua_setfield(L, -2, "getPressed");
    lua_setfield(L, -2, "touch");
    lua_pop(L, 1);
    
    // Implement love.timer.sleep using svcSleepThread
    lua_getglobal(L, "love");
    lua_getfield(L, -1, "timer");
    lua_pushcfunction(L, [](lua_State* L2) -> int {
        float secs = luaL_checknumber(L2, 1);
        if (secs > 0) {
            svcSleepThread((s64)(secs * 1000000000LL));
        }
        return 0;
    });
    lua_setfield(L, -2, "sleep");
    lua_pop(L, 2);

    // Load main.lua
    if (luaL_dofile(L, "romfs:/main.lua") != 0) {
        printf("Error loading main.lua: %s\n", lua_tostring(L, -1));
        printf("\x1b[31mPress START to exit.\x1b[0m\n");
    }

    // Call love.load
    printf("Calling love.load...\n");
    lua_getglobal(L, "love");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "load");
        if (lua_isfunction(L, -1)) {
            // Push arguments if any
            lua_newtable(L); // arg table
            if (lua_pcall(L, 1, 0, 0) != 0) {
                printf("love.load ERROR: %s\n", lua_tostring(L, -1));
                lua_pop(L, 1); // pop error
            } else {
                printf("love.load completed OK\n");
            }
        } else {
            printf("love.load is not a function!\n");
        }
        lua_pop(L, 1); // pop love.load / error
    } else {
        printf("love table not found!\n");
    }
    lua_pop(L, 1); // pop love table
    sys_log("gen1recomp3ds build: APT60 + gpuWait/luaDraw/drawCpp/texSwitches diagnostics + deep audio + btPath/btPics/btHuds/btAnim/btText/btOther battle profile");
    printf("Entering main loop...\n");

    // Init log background thread
    LightLock_Init(&g_log_lock);
    LightEvent_Init(&g_log_event, RESET_STICKY);
    s32 log_prio = 0;
    svcGetThreadPriority(&log_prio, CUR_THREAD_HANDLE);
    g_log_thread = threadCreate(log_thread_func, nullptr, 16 * 1024, log_prio - 1, -1, false);

    // Main loop
    while (aptMainLoop() && !love_input_want_quit()) {
        u64 t0 = osGetTime();

        // reset per-frame draw diagnostics; they accumulate during this
        // frame's update/draw and are logged by the slow-frame line below
        g_dbg_batch_calls = 0;
        g_dbg_batch_entries = 0;
        g_dbg_single_calls = 0;
        g_dbg_batch_ms = 0;
        g_dbg_draw_cpp_ms = 0;
        g_dbg_tex_switches = 0;
        g_last_tex = NULL;
        
        // Push message handler for traceback
        lua_getglobal(L, "debug");
        lua_getfield(L, -1, "traceback");
        lua_remove(L, -2);
        int msgh = lua_gettop(L);

        // Dispatch 3DS buttons / circle pad as love.keypressed/keyreleased
        love_input_poll(L, msgh);

        // Call love.update
        lua_getglobal(L, "love");
        lua_getfield(L, -1, "update");
        if (lua_isfunction(L, -1)) {
            lua_pushnumber(L, 1.0 / 60.0); // dt
            if (lua_pcall(L, 1, 0, msgh) != 0) {
                printf("Error calling love.update:\n%s\n", lua_tostring(L, -1));
                sys_log("Error calling love.update: %s", lua_tostring(L, -1));
                printf("\x1b[31mPress START to exit.\x1b[0m\n");
                // Don't spam errors
                lua_getglobal(L, "love");
                lua_pushnil(L);
                lua_setfield(L, -2, "update");
                lua_pop(L, 1);
            }
        }
        lua_settop(L, msgh); // Restore stack to just msgh

        u64 t1 = osGetTime();

        // Render the scene.  C3D_FrameBegin(SYNCDRAW) BLOCKS until the GPU has
        // finished the previous frame's commands, so time it separately: if the
        // game is GPU-bound this wait is where the frame time goes (and "wait:"
        // below, measured after C3D_FrameEnd, would look ~0 and mislead).
        u64 dbg_gpu0 = osGetTime();
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        u64 dbg_gpu1 = osGetTime();

        // Scale: 160x144 game canvas -> 400x240 top screen
        // Scale factors: 400/160 = 2.5, 240/144 = 1.666...
        // Use uniform scale = min(2.5, 1.666) = 1.666 to preserve aspect ratio,
        // or letterbox. For a pixel-perfect full-height fit: scale = 240/144.
        // Push scale into Lua so love.graphics.draw can apply it.
        const float SCALE_X = 400.0f / 160.0f; // 2.5x
        const float SCALE_Y = 240.0f / 144.0f; // 1.6667x

        // Stereoscopic 3D (glasses-free, 3DS parallax barrier): read the 3D
        // slider.  When it is up, render the whole scene twice -- once per eye
        // -- each with a horizontal parallax offset (g_eyeParallax = +slider
        // left, -slider right); the C++ draw path adds slider *
        // love.graphics._parallax to every draw's X, so layers the Lua draw
        // publishes (tiles recess, sprites pop, UI stays at the screen plane)
        // diverge between eyes and the barrier fuses them into depth.  At
        // slider 0 the scene renders once (mono), exactly as before.
        float slider = osGet3DSliderState();
        lua_getglobal(L, "love");
        lua_getfield(L, -1, "graphics");
        lua_pushnumber(L, slider);
        lua_setfield(L, -2, "_3dSlider");
        lua_pop(L, 2); // pop love.graphics and love

        // Draw the scene for one eye onto `target`.  eye is 0 (left / mono) or
        // 1 (right); eyeParallax is +slider for the left eye and -slider for
        // the right.  Pushes the scale and resets love.graphics._parallax to 0
        // (the Lua draw raises it per layer) before calling love.draw.
        u64 dbg_lua0 = osGetTime();
        auto renderEye = [&](C3D_RenderTarget* target, int eye, float eyeParallax) {
            C2D_TargetClear(target, C2D_Color32(0x00, 0x00, 0x00, 0xFF));
            C2D_SceneBegin(target);
            graphics_setScreenTarget(target);
            graphics_setScreenScale(SCALE_X, SCALE_Y);
            g_eyeParallax = eyeParallax;
            lua_getglobal(L, "love");
            lua_getfield(L, -1, "graphics");
            lua_pushnumber(L, eye);
            lua_setfield(L, -2, "_eye");
            lua_pushnumber(L, 0.0f);
            lua_setfield(L, -2, "_parallax");
            lua_pushnumber(L, SCALE_X);
            lua_setfield(L, -2, "_scaleX");
            lua_pushnumber(L, SCALE_Y);
            lua_setfield(L, -2, "_scaleY");
            lua_pop(L, 2); // pop love.graphics and love

            lua_getglobal(L, "love");
            lua_getfield(L, -1, "draw");
            if (lua_isfunction(L, -1)) {
                if (lua_pcall(L, 0, 0, msgh) != 0) {
                    printf("Error in love.draw:\n%s\n", lua_tostring(L, -1));
                    sys_log("Error in love.draw: %s", lua_tostring(L, -1));
                    // Don't spam errors
                    lua_getglobal(L, "love");
                    lua_pushnil(L);
                    lua_setfield(L, -2, "draw");
                    lua_pop(L, 1);
                }
            } else {
                // love.draw is nil - draw a diagnostic notice
                C2D_DrawRectSolid(5, 25, 0.0f, 100, 10, C2D_Color32(0xFF, 0x00, 0x00, 0xFF));
            }
            lua_settop(L, msgh); // Restore stack to just msgh
        };

        renderEye(top, 0, +slider);
        if (slider > 0.001f) {
            renderEye(topRight, 1, -slider);
        }
        u64 dbg_lua1 = osGetTime();

        // Bottom screen (320x240): the launcher installs love.graphics._drawBottom
        // to render its UI here.  The hook is cleared when a game boots, so the
        // bottom stays black during gameplay (until the two-screen mod arrives).
        // Bottom is single-screen, so parallax / eye are forced to mono.
        {
            C2D_TargetClear(bottom, C2D_Color32(0x00, 0x00, 0x00, 0xFF));
            C2D_SceneBegin(bottom);
            graphics_setScreenTarget(bottom);
            graphics_setScreenScale(1.0f, 1.0f);
            g_eyeParallax = 0.0f;
            lua_getglobal(L, "love");
            lua_getfield(L, -1, "graphics");
            lua_pushnumber(L, 0.0f); lua_setfield(L, -2, "_3dSlider");
            lua_pushnumber(L, 0);    lua_setfield(L, -2, "_eye");
            lua_pushnumber(L, 0.0f); lua_setfield(L, -2, "_parallax");
            lua_pushnumber(L, 1.0f); lua_setfield(L, -2, "_scaleX");
            lua_pushnumber(L, 1.0f); lua_setfield(L, -2, "_scaleY");
            lua_pop(L, 2); // love.graphics, love

            lua_getglobal(L, "love");
            lua_getfield(L, -1, "graphics");
            lua_getfield(L, -1, "_drawBottom");
            if (lua_isfunction(L, -1)) {
                if (lua_pcall(L, 0, 0, msgh) != 0) {
                    printf("Error in love.graphics._drawBottom:\n%s\n", lua_tostring(L, -1));
                    sys_log("Error in love.graphics._drawBottom: %s", lua_tostring(L, -1));
                    // Don't spam errors
                    lua_getglobal(L, "love");
                    lua_getfield(L, -1, "graphics");
                    lua_pushnil(L);
                    lua_setfield(L, -2, "_drawBottom");
                    lua_pop(L, 2);
                }
            }
            lua_settop(L, msgh); // Restore stack to just msgh
        }

        lua_pop(L, 1); // pop the debug.traceback handler

        u64 t2 = osGetTime();

        C3D_FrameEnd(0);

        u64 t3 = osGetTime();

        // The 3DS SD card and the Lua VM are both slow enough that writing a log
        // entry per slow frame amplifies the issue. Keep the diagnostics, but
        // throttle them to a low frequency so a real performance hiccup does not
        // turn into a feedback loop of extra disk I/O.
        if (t2 - t0 > 17) {
            u64 now_ms = osGetTime();
            if (now_ms - g_last_slow_frame_log_ms > 5000ULL) {
                char btPics[16], btHuds[16], btAnim[16], btText[16], btOther[16], btPath[32];
                lua_num_or_dash(L, "_dbg_bt_pics", btPics, sizeof(btPics));
                lua_num_or_dash(L, "_dbg_bt_huds", btHuds, sizeof(btHuds));
                lua_num_or_dash(L, "_dbg_bt_anim", btAnim, sizeof(btAnim));
                lua_num_or_dash(L, "_dbg_bt_text", btText, sizeof(btText));
                lua_num_or_dash(L, "_dbg_bt_other", btOther, sizeof(btOther));
                lua_str_or_dash(L, "_dbg_bt_path", btPath, sizeof(btPath));
                sys_log("Slow frame! update: %llums, draw: %llums, wait: %llums | gpuWait:%llu luaDraw:%llu drawCpp:%llu texSwitches:%u batchCalls:%u batchEntries:%u singleCalls:%u batchMs:%llu slider:%.2f | btPath:%s btPics:%s btHuds:%s btAnim:%s btText:%s btOther:%s",
                        t1 - t0, t2 - t1, t3 - t2,
                        dbg_gpu1 - dbg_gpu0, dbg_lua1 - dbg_lua0, g_dbg_draw_cpp_ms,
                        g_dbg_tex_switches,
                        g_dbg_batch_calls, g_dbg_batch_entries,
                        g_dbg_single_calls, g_dbg_batch_ms,
                        slider,
                        btPath, btPics, btHuds, btAnim, btText, btOther);
                g_last_slow_frame_log_ms = now_ms;
            }
        }
    }

    // Clean up the audio worker thread before shutting down
    const char* quit_script = 
        "if love and love.thread and love.thread.getChannel then "
        "  local ch = love.thread.getChannel('chipaudio_cmd') "
        "  if ch then ch:push('{ cmd=\"quit\" }') end "
        "end";
    luaL_dostring(L, quit_script);
    svcSleepThread(50000000); // 50ms to allow thread to exit

    g_log_quit = true;
    flush_log_buffer();
    if (g_log_thread) {
        threadJoin(g_log_thread, U64_MAX);
        threadFree(g_log_thread);
    }

    lua_close(L);
    ndspExit();
    C2D_Fini();
    C3D_Fini();
    gfxExit();
    romfsExit();
    return 0;
}
