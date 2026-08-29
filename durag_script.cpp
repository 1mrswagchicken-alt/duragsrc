// Durag Script Runtime - Lua 5.4 embedded in the injected DLL.
#include "durag_script.h"
#include "script_api.h"
#include <windows.h>
#include <mutex>
#include <string>
#include <cstdint>
#include <cstdio>
#include <cstring>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

static lua_State* g_L = nullptr;
static std::mutex g_lua_mtx;
static std::string g_out;
static uint64_t g_deadline = 0;
static const uint64_t EXEC_TIMEOUT_MS = 4000;

static void Out(const char* s) {
    if (!s) s = "";
    if (g_out.size() > 16000) return;
    g_out += s;
    g_out += "\n";
}

// ---------------------------------------------------------------- JSON helper
static std::string JsonEscape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if ((unsigned char)c < 0x20) { char b[8]; sprintf_s(b, sizeof(b), "\\u%04x", c); o += b; }
                else o += c;
        }
    }
    return o;
}

// ---------------------------------------------------------------- print capture
static int L_Print(lua_State* L) {
    int n = lua_gettop(L);
    std::string line;
    for (int i = 1; i <= n; i++) {
        size_t len = 0;
        const char* s = luaL_tolstring(L, i, &len);
        if (i > 1) line += "\t";
        if (s && len) line.append(s, len);
        lua_pop(L, 1);
    }
    Out(line.c_str());
    return 0;
}

// ---------------------------------------------------------------- timeout hook
static void TimeoutHook(lua_State* L, lua_Debug*) {
    if (GetTickCount64() > g_deadline) {
        luaL_error(L, "execution timed out (limit %d ms)", (int)EXEC_TIMEOUT_MS);
    }
}

// ---------------------------------------------------------------- Vector3
struct Vec3 { double x, y, z; };

static Vec3 CheckVec3(lua_State* L, int idx) {
    Vec3* v = (Vec3*)luaL_checkudata(L, idx, "Durag.Vector3");
    return *v;
}

static Vec3* PushVec3(lua_State* L, double x, double y, double z) {
    Vec3* v = (Vec3*)lua_newuserdata(L, sizeof(Vec3));
    v->x = x; v->y = y; v->z = z;
    luaL_setmetatable(L, "Durag.Vector3");
    return v;
}

static int Vec3_New(lua_State* L) {
    double x = luaL_optnumber(L, 1, 0);
    double y = luaL_optnumber(L, 2, 0);
    double z = luaL_optnumber(L, 3, 0);
    PushVec3(L, x, y, z);
    return 1;
}

static int Vec3_Index(lua_State* L) {
    Vec3* v = (Vec3*)luaL_checkudata(L, 1, "Durag.Vector3");
    const char* k = luaL_checkstring(L, 2);
    if (!strcmp(k, "x") || !strcmp(k, "X")) { lua_pushnumber(L, v->x); return 1; }
    if (!strcmp(k, "y") || !strcmp(k, "Y")) { lua_pushnumber(L, v->y); return 1; }
    if (!strcmp(k, "z") || !strcmp(k, "Z")) { lua_pushnumber(L, v->z); return 1; }
    lua_getmetatable(L, 1);
    lua_getfield(L, -1, "__methods");
    lua_pushvalue(L, 2);
    lua_rawget(L, -2);
    if (lua_isnil(L, -1)) luaL_error(L, "Vector3 has no field '%s'", k);
    return 1;
}

static int Vec3_NewIndex(lua_State* L) {
    Vec3* v = (Vec3*)luaL_checkudata(L, 1, "Durag.Vector3");
    const char* k = luaL_checkstring(L, 2);
    double val = luaL_checknumber(L, 3);
    if (!strcmp(k, "x") || !strcmp(k, "X")) v->x = val;
    else if (!strcmp(k, "y") || !strcmp(k, "Y")) v->y = val;
    else if (!strcmp(k, "z") || !strcmp(k, "Z")) v->z = val;
    else luaL_error(L, "Vector3 has no writable field '%s'", k);
    return 0;
}

static int Vec3_ToString(lua_State* L) {
    Vec3* v = (Vec3*)luaL_checkudata(L, 1, "Durag.Vector3");
    lua_pushfstring(L, "Vector3(%.6g, %.6g, %.6g)", v->x, v->y, v->z);
    return 1;
}

static int Vec3_Add(lua_State* L) {
    Vec3 a = CheckVec3(L, 1);
    Vec3* r = PushVec3(L, 0, 0, 0);
    if (lua_isnumber(L, 2)) { double s = lua_tonumber(L, 2); *r = { a.x + s, a.y + s, a.z + s }; }
    else { Vec3 b = CheckVec3(L, 2); *r = { a.x + b.x, a.y + b.y, a.z + b.z }; }
    return 1;
}

static int Vec3_Sub(lua_State* L) {
    Vec3 a = CheckVec3(L, 1);
    Vec3* r = PushVec3(L, 0, 0, 0);
    if (lua_isnumber(L, 2)) { double s = lua_tonumber(L, 2); *r = { a.x - s, a.y - s, a.z - s }; }
    else { Vec3 b = CheckVec3(L, 2); *r = { a.x - b.x, a.y - b.y, a.z - b.z }; }
    return 1;
}

static int Vec3_Mul(lua_State* L) {
    Vec3 a = CheckVec3(L, 1);
    Vec3* r = PushVec3(L, 0, 0, 0);
    if (lua_isnumber(L, 2)) { double s = lua_tonumber(L, 2); *r = { a.x * s, a.y * s, a.z * s }; }
    else { Vec3 b = CheckVec3(L, 2); *r = { a.x * b.x, a.y * b.y, a.z * b.z }; }
    return 1;
}

static int Vec3_Div(lua_State* L) {
    Vec3 a = CheckVec3(L, 1);
    double s = luaL_checknumber(L, 2);
    if (s == 0) luaL_error(L, "division by zero");
    PushVec3(L, a.x / s, a.y / s, a.z / s);
    return 1;
}

static int Vec3_Unm(lua_State* L) {
    Vec3 a = CheckVec3(L, 1);
    PushVec3(L, -a.x, -a.y, -a.z);
    return 1;
}

static int Vec3_Eq(lua_State* L) {
    Vec3 a = CheckVec3(L, 1), b = CheckVec3(L, 2);
    lua_pushboolean(L, a.x == b.x && a.y == b.y && a.z == b.z);
    return 1;
}

static int Vec3_Length(lua_State* L) {
    Vec3 v = CheckVec3(L, 1);
    lua_pushnumber(L, sqrt(v.x * v.x + v.y * v.y + v.z * v.z));
    return 1;
}

static int Vec3_Normalized(lua_State* L) {
    Vec3 v = CheckVec3(L, 1);
    double len = sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (len == 0) luaL_error(L, "cannot normalize a zero vector");
    PushVec3(L, v.x / len, v.y / len, v.z / len);
    return 1;
}

static int Vec3_Dot(lua_State* L) {
    Vec3 a = CheckVec3(L, 1), b = CheckVec3(L, 2);
    lua_pushnumber(L, a.x * b.x + a.y * b.y + a.z * b.z);
    return 1;
}

static int Vec3_Cross(lua_State* L) {
    Vec3 a = CheckVec3(L, 1), b = CheckVec3(L, 2);
    PushVec3(L, a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
    return 1;
}

static int Vec3_Lerp(lua_State* L) {
    Vec3 a = CheckVec3(L, 1), b = CheckVec3(L, 2);
    double t = luaL_checknumber(L, 3);
    PushVec3(L, a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t);
    return 1;
}

static void RegisterVector3(lua_State* L) {
    static const luaL_Reg methods[] = {
        {"Dot", Vec3_Dot}, {"Cross", Vec3_Cross},
        {"Magnitude", Vec3_Length}, {"Length", Vec3_Length},
        {"Normalized", Vec3_Normalized}, {"Unit", Vec3_Normalized},
        {"Lerp", Vec3_Lerp},
        {nullptr, nullptr}
    };
    luaL_newmetatable(L, "Durag.Vector3");     // [mt]
    lua_createtable(L, 0, 8);                  // [mt, methods]
    luaL_setfuncs(L, methods, 0);
    lua_setfield(L, -2, "__methods");          // mt.__methods

    lua_pushcfunction(L, Vec3_Index);    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, Vec3_NewIndex); lua_setfield(L, -2, "__newindex");
    lua_pushcfunction(L, Vec3_ToString); lua_setfield(L, -2, "__tostring");
    lua_pushcfunction(L, Vec3_Add);      lua_setfield(L, -2, "__add");
    lua_pushcfunction(L, Vec3_Sub);      lua_setfield(L, -2, "__sub");
    lua_pushcfunction(L, Vec3_Mul);      lua_setfield(L, -2, "__mul");
    lua_pushcfunction(L, Vec3_Div);      lua_setfield(L, -2, "__div");
    lua_pushcfunction(L, Vec3_Unm);      lua_setfield(L, -2, "__unm");
    lua_pushcfunction(L, Vec3_Eq);       lua_setfield(L, -2, "__eq");
    lua_pop(L, 1);

    lua_createtable(L, 0, 3);                  // Vector3 namespace
    lua_pushcfunction(L, Vec3_New); lua_setfield(L, -2, "new");
    PushVec3(L, 0, 0, 0); lua_setfield(L, -2, "zero");
    PushVec3(L, 1, 1, 1); lua_setfield(L, -2, "one");
    lua_setglobal(L, "Vector3");
}

// ---------------------------------------------------------------- player / features
static int L_GetPosition(lua_State* L) {
    float x, y, z;
    if (!g_host.get_self_pos || !g_host.get_self_pos(&x, &y, &z))
        luaL_error(L, "player not captured yet - move around in-game first");
    PushVec3(L, x, y, z);
    return 1;
}

static Vec3 ArgPosition(lua_State* L, int idx) {
    if (lua_isuserdata(L, idx)) return CheckVec3(L, idx);
    Vec3 v{ 0,0,0 };
    v.x = luaL_checknumber(L, idx);
    v.y = luaL_optnumber(L, idx + 1, 0);
    v.z = luaL_optnumber(L, idx + 2, 0);
    return v;
}

static int L_SetPosition(lua_State* L) {
    Vec3 v = ArgPosition(L, 1);
    if (!g_host.set_self_pos((float)v.x, (float)v.y, (float)v.z))
        luaL_error(L, "player not captured yet - move around in-game first");
    lua_pushboolean(L, 1);
    return 1;
}

static int L_GetYaw(lua_State* L) {
    lua_pushnumber(L, g_host.get_self_yaw ? g_host.get_self_yaw() : 0.0);
    return 1;
}

static int L_SetFlag(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TBOOLEAN);
    if (!g_host.set_flag) luaL_error(L, "host api unavailable");
    g_host.set_flag(name, lua_toboolean(L, 2) != 0);
    lua_pushboolean(L, 1);
    return 1;
}

static int L_GetFlag(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    lua_pushboolean(L, g_host.get_flag ? g_host.get_flag(name) : false);
    return 1;
}

static int L_SetMult(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    double v = luaL_checknumber(L, 2);
    if (!g_host.set_mult) luaL_error(L, "host api unavailable");
    g_host.set_mult(name, v);
    lua_pushnumber(L, v);
    return 1;
}

static int L_GetMult(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    lua_pushnumber(L, g_host.get_mult ? g_host.get_mult(name) : 0.0);
    return 1;
}

static int L_StopAll(lua_State* L) {
    if (g_host.stop_all) g_host.stop_all();
    return 0;
}

static int L_AttachWait(lua_State* L) {
    float x, y, z;
    bool ok = g_host.get_self_pos && g_host.get_self_pos(&x, &y, &z);
    lua_pushboolean(L, ok);
    return 1;
}

// ---------------------------------------------------------------- client service
static int L_ClientPid(lua_State* L) {
    lua_pushinteger(L, g_host.self_pid ? (lua_Integer)g_host.self_pid() : 0);
    return 1;
}

static int L_ClientList(lua_State* L) {
    uint32_t pids[64];
    int n = g_host.list_client_pids ? g_host.list_client_pids(pids, 64) : 0;
    lua_createtable(L, n, 0);
    for (int i = 0; i < n; i++) { lua_pushinteger(L, (lua_Integer)pids[i]); lua_rawseti(L, -2, i + 1); }
    return 1;
}

static int L_ClientCount(lua_State* L) {
    uint32_t pids[64];
    int n = g_host.list_client_pids ? g_host.list_client_pids(pids, 64) : 0;
    lua_pushinteger(L, n);
    return 1;
}

// ---------------------------------------------------------------- module service
static int L_ModuleReadFile(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    for (const char* c = name; *c; c++) {
        if (*c == '.' && c[1] == '.') luaL_argerror(L, 1, "invalid module name");
        if (*c == '/' || *c == ':' || *c == '\\') luaL_argerror(L, 1, "invalid module name");
    }
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    if (slash) *(slash + 1) = 0; else path[0] = 0;
    strncat_s(path, "modules\\", sizeof(path) - strlen(path) - 1);
    strncat_s(path, name, sizeof(path) - strlen(path) - 1);
    strncat_s(path, ".lua", sizeof(path) - strlen(path) - 1);
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) { lua_pushnil(L); return 1; }
    std::string out;
    char buf[4096];
    size_t r;
    while ((r = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, r);
    fclose(f);
    lua_pushlstring(L, out.c_str(), out.size());
    return 1;
}

// ---------------------------------------------------------------- init
static void RegisterApi(lua_State* L) {
    luaL_openlibs(L);

    // print -> captured output returned to the UI console
    lua_pushcfunction(L, L_Print);
    lua_setglobal(L, "print");

    // Safety trims
    lua_pushnil(L); lua_setglobal(L, "io");
    lua_pushnil(L); lua_setglobal(L, "dofile");
    lua_pushnil(L); lua_setglobal(L, "loadfile");
    lua_getglobal(L, "os");
    if (lua_istable(L, -1)) {
        lua_pushnil(L); lua_setfield(L, -2, "execute");
        lua_pushnil(L); lua_setfield(L, -2, "exit");
        lua_pushnil(L); lua_setfield(L, -2, "remove");
        lua_pushnil(L); lua_setfield(L, -2, "rename");
    }
    lua_pop(L, 1);

    RegisterVector3(L);

    // durag root
    lua_createtable(L, 0, 8);

    lua_createtable(L, 0, 4);                    // durag.player
    lua_pushcfunction(L, L_GetPosition);         lua_setfield(L, -2, "get_position");
    lua_pushcfunction(L, L_SetPosition);         lua_setfield(L, -2, "set_position");
    lua_pushcfunction(L, L_GetYaw);              lua_setfield(L, -2, "get_yaw");
    lua_setfield(L, -2, "player");

    lua_createtable(L, 0, 9);                    // durag.features
    lua_pushcfunction(L, L_SetFlag);             lua_setfield(L, -2, "fly");
    lua_pushcfunction(L, L_SetFlag);             lua_setfield(L, -2, "noclip");
    lua_pushcfunction(L, L_SetFlag);             lua_setfield(L, -2, "speed");
    lua_pushcfunction(L, L_SetFlag);             lua_setfield(L, -2, "size");
    lua_pushcfunction(L, L_GetFlag);             lua_setfield(L, -2, "get_flag");
    lua_pushcfunction(L, L_SetMult);             lua_setfield(L, -2, "set");
    lua_pushcfunction(L, L_GetMult);             lua_setfield(L, -2, "get");
    lua_pushcfunction(L, L_StopAll);             lua_setfield(L, -2, "stop");
    lua_setfield(L, -2, "features");

    lua_pushcfunction(L, L_SetPosition);         lua_setfield(L, -2, "teleport");

    lua_pushliteral(L, "1.1.0");                 lua_setfield(L, -2, "version");
    lua_pushcfunction(L, L_AttachWait);          lua_setfield(L, -2, "attach_wait");

    lua_createtable(L, 0, 4);                    // durag.client
    lua_pushcfunction(L, L_ClientPid);           lua_setfield(L, -2, "pid");
    lua_pushcfunction(L, L_ClientList);          lua_setfield(L, -2, "list");
    lua_pushcfunction(L, L_ClientCount);         lua_setfield(L, -2, "count");
    lua_setfield(L, -2, "client");

    lua_createtable(L, 0, 4);                    // durag.modules
    lua_createtable(L, 0, 0);                    lua_setfield(L, -2, "_registry");
    lua_createtable(L, 0, 0);                    lua_setfield(L, -2, "_cache");
    lua_pushcfunction(L, L_ModuleReadFile);      lua_setfield(L, -2, "_readfile");
    lua_setfield(L, -2, "modules");

    lua_setglobal(L, "durag");

    // Bootstrap require + module registration in Lua itself
    static const char* BOOTSTRAP =
        "do\n"
        "  local reg, cache, readfile = durag.modules._registry, durag.modules._cache, durag.modules._readfile\n"
        "  local function load_module(name)\n"
        "    local src = reg[name] or readfile(name)\n"
        "    assert(src, \"module '\" .. tostring(name) .. \"' not found\")\n"
        "    local fn, err = load(src, \"@\" .. tostring(name))\n"
        "    assert(fn, err)\n"
        "    return fn()\n"
        "  end\n"
        "  function require(name)\n"
        "    assert(type(name) == \"string\", \"require expects a module name\")\n"
        "    local v = cache[name]\n"
        "    if v ~= nil then return v end\n"
        "    v = load_module(name)\n"
        "    cache[name] = (v == nil) and true or v\n"
        "    return cache[name]\n"
        "  end\n"
        "  function durag.modules.register(name, code)\n"
        "    assert(type(name) == \"string\" and type(code) == \"string\", \"register(name, code)\")\n"
        "    reg[name] = code\n"
        "  end\n"
        "end";
    if (luaL_dostring(L, BOOTSTRAP) != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        OutputDebugStringA("[Durag] script bootstrap failed: ");
        OutputDebugStringA(err ? err : "?");
        OutputDebugStringA("\n");
    }
}

void Script_Init() {
    std::lock_guard<std::mutex> lk(g_lua_mtx);
    if (g_L) return;
    g_L = luaL_newstate();
    if (!g_L) return;
    RegisterApi(g_L);
    OutputDebugStringA("[Durag] Script runtime ready (Lua 5.4)");
}

static std::string Script_ExecLocked(const std::string& code) {
    g_out.clear();
    g_deadline = GetTickCount64() + EXEC_TIMEOUT_MS;

    lua_sethook(g_L, TimeoutHook, LUA_MASKCOUNT, 500000);

    int status = luaL_loadbuffer(g_L, code.c_str(), code.size(), "=script");
    if (status == LUA_OK) status = lua_pcall(g_L, 0, 0, 0);

    lua_sethook(g_L, nullptr, 0, 0);

    if (status != LUA_OK) {
        const char* err = lua_tostring(g_L, -1);
        std::string msg = err ? err : "unknown error";
        lua_pop(g_L, 1);
        return "{\"status\":\"error\",\"msg\":\"" + JsonEscape(msg) + "\"}";
    }

    std::string out = g_out;
    while (out.size() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return "{\"status\":\"ok\",\"msg\":\"" + JsonEscape(out) + "\"}";
}

std::string Script_Exec(const std::string& code) {
    std::lock_guard<std::mutex> lk(g_lua_mtx);
    if (!g_L) return "{\"status\":\"error\",\"msg\":\"script runtime not initialized\"}";
    return Script_ExecLocked(code);
}
