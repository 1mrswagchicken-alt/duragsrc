// =============================================================================
// Durag Executor DLL
// Target: Vortex.exe v0.2.23 (Bevy 0.15+ / Rust)
// Strategy: HWBP for Local Player + WSARecv Hook for Network Entities
// =============================================================================

#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <tlhelp32.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>
#include <mutex>
#include <atomic>
#include <memory>
#include <psapi.h>
#include <timeapi.h>
#include <algorithm>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "ws2_32.lib")

#include "MinHook.h"
#include "sig_scanner.h"
#include "durag_script.h"
#include "script_api.h"

// =============================================================================
// VORTEX NETWORK PROTOCOL DEFINITIONS
// =============================================================================

namespace vx {
    struct NetEntity {
        uint64_t id;
        float x, y, z;
        float yaw;
        bool moving;
        bool grounded;
        bool dead;
        uint64_t last_update;
        char name[32];
        bool has_pos;
    };
}

#define MAX_NET_ENTITIES 64
static vx::NetEntity g_net_entities[MAX_NET_ENTITIES] = { 0 };
static std::atomic<int> g_net_entity_count{ 0 };
static std::mutex g_net_mutex;
static std::atomic<SOCKET> g_game_socket{ INVALID_SOCKET };

static int AllocNetEntitySlot() {
    int count = g_net_entity_count.load();
    if (count < MAX_NET_ENTITIES) return count;
    uint64_t oldest = UINT64_MAX;
    int victim = 0;
    for (int i = 0; i < MAX_NET_ENTITIES; i++) {
        if (g_net_entities[i].last_update < oldest) { oldest = g_net_entities[i].last_update; victim = i; }
    }
    return victim;
}

static void UpdateNetEntity(uint64_t id, float x, float y, float z, float yaw, bool moving, bool grounded, bool dead, const char* name = nullptr) {
    std::lock_guard<std::mutex> lk(g_net_mutex);
    for (int i = 0; i < g_net_entity_count.load(); i++) {
        if (g_net_entities[i].id == id) {
            g_net_entities[i].x = x; g_net_entities[i].y = y; g_net_entities[i].z = z;
            g_net_entities[i].yaw = yaw;
            g_net_entities[i].moving = moving;
            g_net_entities[i].grounded = grounded;
            g_net_entities[i].dead = dead;
            g_net_entities[i].last_update = GetTickCount64();
            if (name && strlen(name) > 0) {
                strncpy_s(g_net_entities[i].name, sizeof(g_net_entities[i].name), name, _TRUNCATE);
            }
            return;
        }
    }
    int idx = AllocNetEntitySlot();
    g_net_entities[idx].id = id;
    g_net_entities[idx].x = x; g_net_entities[idx].y = y; g_net_entities[idx].z = z;
    g_net_entities[idx].yaw = yaw;
    g_net_entities[idx].moving = moving;
    g_net_entities[idx].grounded = grounded;
    g_net_entities[idx].dead = dead;
    g_net_entities[idx].last_update = GetTickCount64();
    if (name && strlen(name) > 0) {
        strncpy_s(g_net_entities[idx].name, sizeof(g_net_entities[idx].name), name, _TRUNCATE);
    }
    else {
        g_net_entities[idx].name[0] = 0;
    }
    g_net_entities[idx].has_pos = true;
    if (idx == g_net_entity_count.load()) g_net_entity_count.store(idx + 1);
}

static void UpdateNetEntityName(uint64_t id, const char* name) {
    std::lock_guard<std::mutex> lk(g_net_mutex);
    for (int i = 0; i < g_net_entity_count.load(); i++) {
        if (g_net_entities[i].id == id) {
            if (name && strlen(name) > 0) {
                strncpy_s(g_net_entities[i].name, sizeof(g_net_entities[i].name), name, _TRUNCATE);
            }
            g_net_entities[i].last_update = GetTickCount64();
            return;
        }
    }
    int idx = AllocNetEntitySlot();
    ZeroMemory(&g_net_entities[idx], sizeof(vx::NetEntity));
    g_net_entities[idx].id = id;
    if (name && strlen(name) > 0) {
        strncpy_s(g_net_entities[idx].name, sizeof(g_net_entities[idx].name), name, _TRUNCATE);
    }
    g_net_entities[idx].has_pos = false;
    g_net_entities[idx].last_update = GetTickCount64();
    if (idx == g_net_entity_count.load()) g_net_entity_count.store(idx + 1);
}

// =============================================================================
// GLOBAL STATE
// =============================================================================

static HMODULE g_hModule = nullptr;
static std::atomic<bool> g_initialized{ false };
static std::atomic<bool> g_ipc_running{ false };
static std::atomic<bool> g_shutdown{ false };
static std::atomic<bool> g_movement_thread_running{ true };

static std::atomic<uintptr_t> g_player_base{ 0 };
static std::atomic<bool> g_player_locked{ false };
static std::atomic<int> g_base_fail_count{ 0 };
static std::atomic<float> g_self_x{ 0 };
static std::atomic<float> g_self_y{ 0 };
static std::atomic<float> g_self_z{ 0 };
static std::atomic<float> g_self_yaw{ 0 };
static std::atomic<uint32_t> g_hwbp_threads{ 0 };
static std::atomic<uint32_t> g_hwbp_armed{ 0 };
static std::atomic<uint32_t> g_hwbp_failed{ 0 };
static std::atomic<uint64_t> g_step_hits{ 0 };
static std::atomic<uint64_t> g_self_tick{ 0 };
static std::atomic<bool> g_scanning{ false };
static std::atomic<uint32_t> g_scan_found{ 0 };
static std::atomic<bool> g_manual_base{ false };

static std::atomic<bool> g_fly_enabled{ false };
static std::atomic<float> g_fly_speed{ 50.0f };
static std::atomic<bool> g_speed_enabled{ false };
static std::atomic<float> g_speed_mult{ 2.5f };
static std::atomic<bool> g_noclip_enabled{ false };
static std::atomic<uintptr_t> g_camera_base{ 0 };
static std::atomic<bool> g_view_fly{ false };
static std::atomic<bool> g_size_enabled{ false };
static std::atomic<float> g_size{ 1.0f };

static std::atomic<float> g_target_x{ 0 };
static std::atomic<float> g_target_y{ 0 };
static std::atomic<float> g_target_z{ 0 };
static std::atomic<bool> g_has_target{ false };

static uintptr_t g_write_hook_addr = 0;
// x64 register index holding the player pointer at the write site (1=rcx, 11=r11, ...)
static std::atomic<int> g_hwbp_reg{ -1 };
// HWBP capture candidates are staged here and only locked after verifying
// their [ptr+0..12] floats match the network-reported self position.
static std::atomic<uintptr_t> g_hwbp_candidate{ 0 };
static std::atomic<int> g_hwbp_cand_ok{ 0 };
static std::atomic<int> g_hwbp_cand_bad{ 0 };
// Offset of the XYZ transform inside the captured struct (0 = ptr is the transform)
static std::atomic<int> g_pos_offset{ 0 };
static std::atomic<bool> g_veh_installed{ false };

static std::atomic<bool> g_orbit_enabled{ false };
static std::atomic<float> g_orbit_speed{ 2.0f };
static std::atomic<float> g_orbit_radius{ 5.0f };
static std::atomic<int> g_orbit_target{ -1 };
static double g_orbit_angle = 0.0;
static std::atomic<float> g_orbit_phase{ 0.0f };
static std::atomic<bool> g_goto_enabled{ false };
static std::atomic<float> g_goto_x{ 0 };
static std::atomic<float> g_goto_y{ 0 };
static std::atomic<float> g_goto_z{ 0 };
static std::atomic<float> g_goto_speed{ 6.0f };
static std::atomic<float> g_goto_stop{ 1.0f };
static std::atomic<bool> g_slot_enabled{ false };
static std::atomic<float> g_slot_x{ 0 };
static std::atomic<float> g_slot_y{ 0 };
static std::atomic<float> g_slot_z{ 0 };
static std::atomic<float> g_slot_vx{ 0 };
static std::atomic<float> g_slot_vy{ 0 };
static std::atomic<float> g_slot_vz{ 0 };
static std::atomic<uint64_t> g_slot_tick{ 0 };

static void* g_swap_chain_vtable[18] = { 0 };
static uintptr_t g_present_addr = 0;
typedef HRESULT(__stdcall* PresentFn)(IDXGISwapChain*, UINT, UINT);
static PresentFn g_original_present = nullptr;

// =============================================================================
// SAFE MEMORY HELPERS
// =============================================================================

static bool safe_read_floats(uintptr_t addr, float* out, int count) {
    __try { float* src = (float*)addr; for (int i = 0; i < count; ++i) out[i] = src[i]; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool safe_write_float(uintptr_t addr, float val) {
    __try { *(volatile float*)addr = val; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static void apply_scale(uintptr_t base) {
    float s = g_size.load();
    safe_write_float(base + 0x0C, s);
    safe_write_float(base + 0x10, s);
    safe_write_float(base + 0x14, s);
}

// =============================================================================
// PACKET PROCESSING
// =============================================================================

struct PacketLogEntry { uint64_t tick; uint8_t op; uint32_t len; uint8_t dir; char desc[80]; };
static std::atomic<bool> g_packet_log_enabled{ false };
static std::mutex g_packet_log_mutex;
static std::vector<PacketLogEntry> g_packet_log_entries;

static void RecordPacket(uint8_t op, uint32_t len, uint8_t dir, const char* desc) {
    if (!g_packet_log_enabled.load()) return;
    PacketLogEntry e;
    e.tick = GetTickCount64();
    e.op = op;
    e.len = len;
    e.dir = dir;
    strncpy_s(e.desc, sizeof(e.desc), desc ? desc : "", _TRUNCATE);
    std::lock_guard<std::mutex> lk(g_packet_log_mutex);
    if (g_packet_log_entries.size() >= 128) g_packet_log_entries.erase(g_packet_log_entries.begin());
    g_packet_log_entries.push_back(e);
}

static void BuildPacketDesc(const uint8_t* data, size_t len, uint8_t opcode, char* out, size_t outsz) {
    out[0] = 0;
    if (opcode == 9 && len >= 8) {
        uint32_t cnt = 0;
        memcpy(&cnt, data + 4, 4);
        snprintf(out, outsz, "ent=%u", cnt);
        return;
    }
    if (opcode == 2 && len >= 20) {
        uint32_t nlen = 0;
        memcpy(&nlen, data + 12, 4);
        if (nlen > 0 && nlen < 32 && 20 + nlen <= len) {
            size_t dn = (size_t)snprintf(out, outsz, "name=");
            for (uint32_t k = 0; k < nlen && dn + 1 < outsz; k++) {
                char c = (char)data[20 + k];
                if (c == '"' || c == '\\' || (unsigned char)c < 0x20 || (unsigned char)c > 0x7E) c = '?';
                out[dn++] = c;
            }
            out[dn] = 0;
        }
        else snprintf(out, outsz, "nameLen=%u", nlen);
        return;
    }
    size_t dn = 0;
    size_t hx = len < 32 ? len : 32;
    for (size_t k = 0; k < hx && dn + 3 < outsz; k++) dn += (size_t)snprintf(out + dn, outsz - dn, "%02X", data[k]);
}

static void ProcessInboundPacket(const uint8_t* data, size_t len) {
    if (len < 1) return;
    uint8_t opcode = data[0];

    if (opcode == 9 && len >= 40) {
        uint32_t entityCount = 0;
        memcpy(&entityCount, data + 4, 4);

        size_t offset = 12;

        for (uint32_t i = 0; i < entityCount && offset + 28 <= len; i++) {
            uint64_t entityId = 0;
            memcpy(&entityId, data + offset, 8);

            float x, y, z, yaw;
            memcpy(&x, data + offset + 8, 4);
            memcpy(&y, data + offset + 12, 4);
            memcpy(&z, data + offset + 16, 4);
            memcpy(&yaw, data + offset + 20, 4);

            uint8_t moving = data[offset + 24];
            uint8_t grounded = data[offset + 25];
            uint8_t dead = data[offset + 26];

            if (std::isfinite(x) && std::isfinite(y) && std::isfinite(z) &&
                std::abs(x) < 100000.0f && std::abs(y) < 100000.0f && std::abs(z) < 100000.0f) {
                UpdateNetEntity(entityId, x, y, z, yaw, moving != 0, grounded != 0, dead != 0);
            }
            offset += 27;
        }
    }

    if (opcode == 2 && len >= 20) {
        uint32_t playerId = 0;
        memcpy(&playerId, data + 4, 4);
        uint32_t nameLen = 0;
        memcpy(&nameLen, data + 12, 4);
        if (nameLen > 0 && nameLen < 32 && 20 + nameLen <= len) {
            char name[33] = { 0 };
            memcpy(name, data + 20, nameLen);
            name[nameLen] = '\0';
            char dbg[128];
            snprintf(dbg, sizeof(dbg), "[Durag] Player: %s (id=%u)", name, playerId);
            OutputDebugStringA(dbg);
            UpdateNetEntityName(playerId, name);
        }
    }

    if (g_packet_log_enabled.load()) {
        char pdesc[80] = { 0 };
        BuildPacketDesc(data, len, opcode, pdesc, sizeof(pdesc));
        RecordPacket(opcode, (uint32_t)len, 0, pdesc);
    }
}

static const size_t STREAM_RESYNC = (size_t)-1;

static size_t FrameLength(const uint8_t* buf, size_t avail) {
    if (avail < 1) return 0;
    uint8_t opcode = buf[0];
    if (opcode == 9) {
        if (avail < 12) return 0;
        uint32_t count = 0;
        memcpy(&count, buf + 4, 4);
        if (count > 4096) return STREAM_RESYNC;
        size_t total = 12 + (size_t)count * 27;
        if (avail < total) return 0;
        return total;
    }
    if (opcode == 2) {
        if (avail < 20) return 0;
        uint32_t nameLen = 0;
        memcpy(&nameLen, buf + 12, 4);
        if (nameLen > 4096) return STREAM_RESYNC;
        size_t total = 20 + nameLen;
        if (avail < total) return 0;
        return total;
    }
    return STREAM_RESYNC;
}

struct StreamAccum { SOCKET s; std::vector<uint8_t> buf; };
static std::mutex g_stream_mutex;
static std::vector<StreamAccum> g_streams;

static void ProcessSocketData(SOCKET s, const uint8_t* data, int len) {
    if (len <= 0) return;
    DWORD type = 0;
    int optlen = sizeof(type);
    bool is_stream = getsockopt(s, SOL_SOCKET, SO_TYPE, (char*)&type, &optlen) == 0 && type == SOCK_STREAM;
    if (!is_stream) {
        ProcessInboundPacket(data, (size_t)len);
        return;
    }
    std::lock_guard<std::mutex> lk(g_stream_mutex);
    StreamAccum* acc = nullptr;
    for (auto& st : g_streams) {
        if (st.s == s) { acc = &st; break; }
    }
    if (!acc) {
        if (g_streams.size() < 32) {
            g_streams.push_back(StreamAccum{ s, std::vector<uint8_t>() });
            acc = &g_streams.back();
        }
        else {
            acc = &g_streams[0];
            acc->buf.clear();
            acc->s = s;
        }
    }
    acc->buf.insert(acc->buf.end(), data, data + len);
    static std::atomic<int> g_resync_logs{ 0 };
    size_t off = 0;
    while (off < acc->buf.size()) {
        size_t n = FrameLength(acc->buf.data() + off, acc->buf.size() - off);
        if (n == 0) break;
        if (n == STREAM_RESYNC) {
            off += 1;
            if (g_resync_logs.fetch_add(1) < 10) OutputDebugStringA("[Durag] Stream resync (unknown byte skipped)");
            continue;
        }
        uint8_t fop = acc->buf.data()[off];
        if (fop == 9 || fop == 2) g_game_socket.store(s);
        ProcessInboundPacket(acc->buf.data() + off, n);
        off += n;
    }
    if (off > 0 && off <= acc->buf.size()) acc->buf.erase(acc->buf.begin(), acc->buf.begin() + off);
    if (acc->buf.size() > 262144) acc->buf.clear();
}

// =============================================================================
// WINSOCK HOOKS
// =============================================================================

typedef int (WSAAPI* WSARecvFn)(SOCKET, LPWSABUF, DWORD, LPDWORD, LPDWORD, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
static WSARecvFn g_original_WSARecv = nullptr;

typedef int (WSAAPI* RecvFn)(SOCKET, char*, int, int);
static RecvFn g_original_recv = nullptr;

int WSAAPI Hooked_WSARecv(SOCKET s, LPWSABUF lpBuffers, DWORD dwBufferCount, LPDWORD lpNumberOfBytesRecvd, LPDWORD lpFlags, LPWSAOVERLAPPED lpOverlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine) {
    if (g_game_socket.load() == INVALID_SOCKET) g_game_socket.store(s);
    int result = g_original_WSARecv(s, lpBuffers, dwBufferCount, lpNumberOfBytesRecvd, lpFlags, lpOverlapped, lpCompletionRoutine);
    if (result == 0 && lpNumberOfBytesRecvd && *lpNumberOfBytesRecvd > 0 && lpBuffers && dwBufferCount > 0) {
        ProcessSocketData(s, (const uint8_t*)lpBuffers[0].buf, (int)*lpNumberOfBytesRecvd);
    }
    return result;
}

int WSAAPI Hooked_recv(SOCKET s, char* buf, int len, int flags) {
    if (g_game_socket.load() == INVALID_SOCKET) g_game_socket.store(s);
    int result = g_original_recv(s, buf, len, flags);
    if (result > 0) {
        ProcessSocketData(s, (const uint8_t*)buf, result);
    }
    return result;
}

typedef int (WSAAPI* SendFn)(SOCKET, const char*, int, int);
static SendFn g_original_send = nullptr;

typedef int (WSAAPI* WSASendFn)(SOCKET, LPWSABUF, DWORD, LPDWORD, DWORD, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
static WSASendFn g_original_WSASend = nullptr;

static void LogOutbound(SOCKET s, const char* buf, int len) {
    if (!buf || len <= 0) return;
    if ((uint8_t)buf[0] == 8 && len >= 20) {
        float x, y, z, yaw;
        memcpy(&x, buf + 4, 4);
        memcpy(&y, buf + 8, 4);
        memcpy(&z, buf + 12, 4);
        memcpy(&yaw, buf + 16, 4);
        if (std::isfinite(x) && std::isfinite(y) && std::isfinite(z) &&
            std::abs(x) < 100000.0f && std::abs(y) < 100000.0f && std::abs(z) < 100000.0f) {
            g_self_x.store(x, std::memory_order_relaxed);
            g_self_y.store(y, std::memory_order_relaxed);
            g_self_z.store(z, std::memory_order_relaxed);
            g_self_yaw.store(yaw, std::memory_order_relaxed);
            g_self_tick.store(GetTickCount64(), std::memory_order_relaxed);
            g_game_socket.store(s);
        }
    }
    if (!g_packet_log_enabled.load()) return;
    char pdesc[80] = { 0 };
    size_t dn = 0;
    size_t hx = (size_t)len < 32 ? (size_t)len : 32;
    for (size_t k = 0; k < hx && dn + 3 < sizeof(pdesc); k++) dn += (size_t)snprintf(pdesc + dn, sizeof(pdesc) - dn, "%02X", (uint8_t)buf[k]);
    RecordPacket((uint8_t)buf[0], (uint32_t)len, 1, pdesc);
}

int WSAAPI Hooked_send(SOCKET s, const char* buf, int len, int flags) {
    LogOutbound(s, buf, len);
    return g_original_send(s, buf, len, flags);
}

int WSAAPI Hooked_WSASend(SOCKET s, LPWSABUF lpBuffers, DWORD dwBufferCount, LPDWORD lpNumberOfBytesSent, DWORD dwFlags, LPWSAOVERLAPPED lpOverlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine) {
    if (lpBuffers && dwBufferCount > 0) LogOutbound(s, lpBuffers[0].buf, (int)lpBuffers[0].len);
    return g_original_WSASend(s, lpBuffers, dwBufferCount, lpNumberOfBytesSent, dwFlags, lpOverlapped, lpCompletionRoutine);
}

bool hook_winsock() {
    HMODULE hWs2_32 = GetModuleHandleA("ws2_32.dll");
    if (!hWs2_32) hWs2_32 = LoadLibraryA("ws2_32.dll");
    if (!hWs2_32) return false;
    bool success = true;
    FARPROC addr_recv = GetProcAddress(hWs2_32, "recv");
    if (addr_recv) {
        if (MH_CreateHook((LPVOID)addr_recv, reinterpret_cast<LPVOID>(&Hooked_recv), reinterpret_cast<LPVOID*>(&g_original_recv)) != MH_OK) success = false;
        if (MH_EnableHook((LPVOID)addr_recv) != MH_OK) success = false;
    }
    FARPROC addr_wsarecv = GetProcAddress(hWs2_32, "WSARecv");
    if (addr_wsarecv) {
        if (MH_CreateHook((LPVOID)addr_wsarecv, reinterpret_cast<LPVOID>(&Hooked_WSARecv), reinterpret_cast<LPVOID*>(&g_original_WSARecv)) != MH_OK) success = false;
        if (MH_EnableHook((LPVOID)addr_wsarecv) != MH_OK) success = false;
    }
    FARPROC addr_send = GetProcAddress(hWs2_32, "send");
    if (addr_send) {
        if (MH_CreateHook((LPVOID)addr_send, reinterpret_cast<LPVOID>(&Hooked_send), reinterpret_cast<LPVOID*>(&g_original_send)) != MH_OK) success = false;
        if (MH_EnableHook((LPVOID)addr_send) != MH_OK) success = false;
    }
    FARPROC addr_wsasend = GetProcAddress(hWs2_32, "WSASend");
    if (addr_wsasend) {
        if (MH_CreateHook((LPVOID)addr_wsasend, reinterpret_cast<LPVOID>(&Hooked_WSASend), reinterpret_cast<LPVOID*>(&g_original_WSASend)) != MH_OK) success = false;
        if (MH_EnableHook((LPVOID)addr_wsasend) != MH_OK) success = false;
    }
    if (success) OutputDebugStringA("[Durag] Winsock hooked");
    return success;
}

// =============================================================================
// HARDWARE BREAKPOINT
// =============================================================================

LONG WINAPI VectoredHandler(PEXCEPTION_POINTERS pExceptionInfo) {
    if (pExceptionInfo->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP) {
        if (pExceptionInfo->ContextRecord->Rip == g_write_hook_addr) {
            g_step_hits.fetch_add(1, std::memory_order_relaxed);
            static std::atomic<int> hit_logs{ 0 };
            if (hit_logs.fetch_add(1) < 3) OutputDebugStringA("[Durag] HWBP hit - Rip matched");
            // g_hwbp_reg holds the x64 register index holding the player pointer
            uintptr_t ptr = 0;
            switch (g_hwbp_reg.load(std::memory_order_relaxed)) {
                case 0: ptr = pExceptionInfo->ContextRecord->Rax; break;
                case 1: ptr = pExceptionInfo->ContextRecord->Rcx; break;
                case 2: ptr = pExceptionInfo->ContextRecord->Rdx; break;
                case 3: ptr = pExceptionInfo->ContextRecord->Rbx; break;
                case 5: ptr = pExceptionInfo->ContextRecord->Rbp; break;
                case 6: ptr = pExceptionInfo->ContextRecord->Rsi; break;
                case 7: ptr = pExceptionInfo->ContextRecord->Rdi; break;
                case 8: ptr = pExceptionInfo->ContextRecord->R8; break;
                case 9: ptr = pExceptionInfo->ContextRecord->R9; break;
                case 10: ptr = pExceptionInfo->ContextRecord->R10; break;
                case 11: ptr = pExceptionInfo->ContextRecord->R11; break;
                case 12: ptr = pExceptionInfo->ContextRecord->R12; break;
                case 13: ptr = pExceptionInfo->ContextRecord->R13; break;
                case 14: ptr = pExceptionInfo->ContextRecord->R14; break;
                case 15: ptr = pExceptionInfo->ContextRecord->R15; break;
            }
            if (ptr) {
                // Stage as candidate - locking happens only after position verification
                if (!g_player_locked.load()) {
                    uintptr_t prev = g_hwbp_candidate.exchange(ptr);
                    if (prev != ptr) { g_hwbp_cand_ok.store(0); g_hwbp_cand_bad.store(0); }
                }
            }
            pExceptionInfo->ContextRecord->EFlags |= 0x10000;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void SetHardwareBreakpointOnAllThreads(uintptr_t addr, bool only_new) {
    g_write_hook_addr = addr;
    if (!g_veh_installed.exchange(true)) AddVectoredExceptionHandler(1, VectoredHandler);
    DWORD current_tid = GetCurrentThreadId();
    DWORD pid = GetCurrentProcessId();
    uint32_t threads = 0, armed = 0, failed = 0, skipped = 0;
    __try {
        HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, pid);
        if (hSnapshot == INVALID_HANDLE_VALUE) return;
        THREADENTRY32 te32; te32.dwSize = sizeof(THREADENTRY32);
        if (Thread32First(hSnapshot, &te32)) {
            do {
                if (te32.th32OwnerProcessID != pid) continue;
                if (te32.th32ThreadID == current_tid) continue;
                threads++;
                HANDLE hThread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, te32.th32ThreadID);
                if (!hThread) { failed++; continue; }
                __try {
                    CONTEXT ctx = { 0 }; ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                    if (GetThreadContext(hThread, &ctx)) {
                        if (only_new && ctx.Dr0 == addr && (ctx.Dr7 & 0x1)) { skipped++; CloseHandle(hThread); continue; }
                        ctx.Dr0 = addr; ctx.Dr7 = (ctx.Dr7 & ~0xF) | 0x1;
                        if (SetThreadContext(hThread, &ctx)) armed++; else failed++;
                    }
                    else failed++;
                }
                __except (EXCEPTION_EXECUTE_HANDLER) { failed++; }
                CloseHandle(hThread);
            } while (Thread32Next(hSnapshot, &te32));
        }
        CloseHandle(hSnapshot);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    g_hwbp_threads.store(threads);
    g_hwbp_armed.store(armed);
    g_hwbp_failed.store(failed);
    char dbg[160];
    snprintf(dbg, sizeof(dbg), "[Durag] HWBP pass (%s): addr=base+0x%llX threads=%u armed=%u already=%u failed=%u",
        only_new ? "watchdog" : "initial", (unsigned long long)(addr - (uintptr_t)GetModuleHandleA(nullptr)), threads, armed, skipped, failed);
    OutputDebugStringA(dbg);
}

// --- generic write-site finder ---------------------------------------------
// x86 register order used for CONTEXT mapping: 0=rax 1=rcx 2=rdx 3=rbx 4=rsp
// 5=rbp 6=rsi 7=rdi 8..15=r8..r15
static const char* RegName(int idx) {
    static const char* n[] = { "rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi",
        "r8","r9","r10","r11","r12","r13","r14","r15" };
    return (idx >= 0 && idx < 16) ? n[idx] : "?";
}

// Decode "mov [r64+disp], eax/rax" (opcode 89 /r, reg field must be 0).
// Returns instruction length or 0. regOut = base register index, is64 = REX.W set,
// dispOut = displacement value (transform offset within the struct).
static int DecodeRmwStore(const uint8_t* p, size_t remain, int* regOut, int* is64, int* dispOut) {
    size_t i = 0;
    int rex = 0;
    if (remain < 2) return 0;
    if (p[0] >= 0x40 && p[0] <= 0x4F) { rex = p[0]; i = 1; }
    if (p[i] != 0x89) return 0;
    uint8_t modrm = p[i + 1];
    uint8_t mod = modrm >> 6, reg = (modrm >> 3) & 7, rm = modrm & 7;
    if (reg != 0) return 0;                      // source must be eax/rax
    if (mod == 3) return 0;                      // register form, not a store
    if (mod == 0 && rm == 5) return 0;           // rip-relative
    if (mod == 0 && rm == 4) return 0;           // sib (unhandled)
    int b = rm | ((rex & 1) ? 8 : 0);
    int w = (rex & 8) ? 1 : 0;
    size_t dispSize = (mod == 1) ? 1 : (mod == 2) ? 4 : 0;
    if (i + 2 + dispSize > remain) return 0;
    int disp = 0;
    if (dispSize == 1) disp = (int8_t)p[i + 2];
    else if (dispSize == 4) memcpy(&disp, p + i + 2, 4);
    *regOut = b; *is64 = w; *dispOut = disp;
    return (int)(i + 2 + dispSize);
}

// Decode "mov rax, [rsp+disp8/32]" -> length or 0
static int DecodeRaxFromRsp(const uint8_t* p, size_t remain) {
    if (remain < 5) return 0;
    if (p[0] != 0x48 || p[1] != 0x8B) return 0;
    uint8_t modrm = p[2];
    int mod = modrm >> 6, reg = (modrm >> 3) & 7, rm = modrm & 7;
    if (reg != 0 || rm != 4) return 0;
    if (remain < 4 || p[3] != 0x24) return 0;
    if (mod == 1 && remain >= 5) return 5;
    if (mod == 2 && remain >= 8) return 8;
    return 0;
}

// Find the transform write site by instruction SHAPE instead of exact bytes:
//   mov [reg+d], eax        (32-bit component write)
//   mov rax, [rsp+d]        (stack reload)
//   mov [reg+d], rax        (64-bit write through the SAME pointer register)
// Returns address of the final instruction (breakpoint lands here), or null.
// regOut = pointer register, posOut = struct offset where the transform starts.
static uint8_t* FindWriteSiteGeneric(HMODULE h, int* regOut, int* posOut) {
    if (!h) return nullptr;
    auto base = (uint8_t*)h;
    auto dos = (IMAGE_DOS_HEADER*)base;
    auto nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int s = 0; s < nt->FileHeader.NumberOfSections; s++) {
        auto& sc = sec[s];
        if (!(sc.Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uint8_t* start = base + sc.VirtualAddress;
        size_t size = sc.Misc.VirtualSize;
        if (size < 32) continue;
        for (size_t i = 0; i + 24 < size; i++) {
            int regA = -1, wA = 0, dispA = 0;
            int lenA = DecodeRmwStore(start + i, size - i, &regA, &wA, &dispA);
            if (!lenA || wA) continue;                     // A: 32-bit eax store
            for (int gap = 0; gap <= 12; gap++) {
                size_t j = i + (size_t)lenA + (size_t)gap;
                int lenB = DecodeRaxFromRsp(start + j, size - j);
                if (!lenB) continue;
                int regC = -1, wC = 0, dispC = 0;
                int lenC = DecodeRmwStore(start + j + lenB, size - j - lenB, &regC, &wC, &dispC);
                if (!lenC || !wC) continue;                // C: 64-bit rax store
                if (regC != regA) continue;                // same pointer register
                if (regC == 4) continue;                   // rsp is never the player ptr
                *regOut = regC;
                *posOut = dispC;                           // rax writes 2 floats starting at disp
                return start + j + lenB;                   // breakpoint on C
            }
        }
    }
    return nullptr;
}

void InstallWriteHook() {
    HMODULE h_exe = GetModuleHandleA(nullptr);
    if (!h_exe) return;
    uintptr_t base = (uintptr_t)h_exe;
    // Exact signatures first (fast + precise), generic shape scan as last resort.
    // [pattern, breakpoint offset, pointer register index, transform offset in struct]
    struct SigVariant { const char* pattern; int bpOff; int reg; int posOff; };
    static const SigVariant SIGS[] = {
        // Post-update site: mov [r11+18],eax | mov rax,[rsp+F0] | mov [r11+10],rax
        // rax store covers X(+16),Y(+20); eax writes Z(+24) -> transform at +16
        { "41 89 43 ?? 48 8B 84 24 ?? ?? ?? ?? 49 89 43 ??", 12, 11, 16 },
        // Legacy site: mov rcx,[rsp+off32] | mov [rcx+8],eax | mov rax,[rsp+off8] | mov [rcx],rax
        { "48 8B 8C 24 ?? ?? ?? ?? 89 41 08 48 8B 44 24 ?? 48 89 01", 16, 1, 0 },
    };
    uint8_t* hit = nullptr;
    int bp_off = 0, reg = 0, pos_off = 0;
    for (const auto& sv : SIGS) {
        hit = ScanModule(h_exe, sv.pattern);
        if (hit) { bp_off = sv.bpOff; reg = sv.reg; pos_off = sv.posOff; break; }
    }
    const char* how = "sig";
    if (!hit) {
        int genPos = 0;
        hit = FindWriteSiteGeneric(h_exe, &reg, &genPos);
        bp_off = 0;
        pos_off = genPos;
        how = "shape-scan";
    }
    if (!hit) {
        OutputDebugStringA("[Durag] PLAYER_WRITE_SIG not found - HWBP capture unavailable, scan fallback remains");
        return;
    }
    g_hwbp_reg.store(reg, std::memory_order_relaxed);
    g_pos_offset.store(pos_off, std::memory_order_relaxed);
    uintptr_t target = (uintptr_t)hit + bp_off;
    char dbg[160];
    snprintf(dbg, sizeof(dbg), "[Durag] Write site (%s) @ +0x%llX, breakpoint @ +0x%llX (reg=%s, transform=+%d)",
        how, (unsigned long long)((uintptr_t)hit - base), (unsigned long long)(target - base), RegName(reg), pos_off);
    OutputDebugStringA(dbg);
    SetHardwareBreakpointOnAllThreads(target, false);
}

// =============================================================================
// TRANSFORM SCAN CAPTURE
// =============================================================================

struct TransformCand { uintptr_t addr; float x, y, z; };

static int ScanForTransformCandidates(TransformCand* out, int maxOut, bool relaxed) {
    int count = 0;
    float sx = g_self_x.load(), sy = g_self_y.load(), sz = g_self_z.load();
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const uint8_t* addr = (const uint8_t*)si.lpMinimumApplicationAddress;
    const uint8_t* maxAddr = (const uint8_t*)si.lpMaximumApplicationAddress;
    MEMORY_BASIC_INFORMATION mbi;
    static uint8_t buf[262144];
    while (addr < maxAddr && count < maxOut) {
        if (VirtualQuery(addr, &mbi, sizeof(mbi)) == 0) break;
        bool regionOk = mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) &&
            (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY)) &&
            (mbi.Type == MEM_PRIVATE || (relaxed && mbi.Type == MEM_MAPPED));
        if (regionOk) {
            const uint8_t* base = (const uint8_t*)mbi.BaseAddress;
            size_t regionSize = mbi.RegionSize;
            size_t off = 0;
            while (off < regionSize && count < maxOut) {
                size_t chunk = regionSize - off;
                if (chunk > sizeof(buf)) chunk = sizeof(buf);
                const uint8_t* src = base + off;
                bool ok = true;
                __try { memcpy(buf, src, chunk); } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
                if (ok) {
                    for (size_t i = 0; i + 40 <= chunk; i += 4) {
                        float px, py, pz;
                        memcpy(&px, buf + i, 4); memcpy(&py, buf + i + 4, 4); memcpy(&pz, buf + i + 8, 4);
                        if (!std::isfinite(px) || !std::isfinite(py) || !std::isfinite(pz)) continue;
                        float tol = relaxed ? 2.5f : 1.5f;
                        if (std::abs(px - sx) > tol || std::abs(py - sy) > tol || std::abs(pz - sz) > tol) continue;
                        if (!relaxed) {
                            float s1, s2, s3;
                            memcpy(&s1, buf + i + 12, 4); memcpy(&s2, buf + i + 16, 4); memcpy(&s3, buf + i + 20, 4);
                            if (!std::isfinite(s1) || s1 < 0.2f || s1 > 5.0f) continue;
                            if (std::abs(s1 - s2) > 0.02f || std::abs(s1 - s3) > 0.02f) continue;
                        }
                        out[count].addr = (uintptr_t)(src + i);
                        out[count].x = px; out[count].y = py; out[count].z = pz;
                        count++;
                        if (count >= maxOut) break;
                    }
                }
                off += (chunk > 40 ? chunk - 40 : chunk);
            }
        }
        addr = (const uint8_t*)mbi.BaseAddress + mbi.RegionSize;
    }
    return count;
}

static void TryScanCapture() {
    if (g_manual_base.load()) return;
    if (g_player_locked.load() || g_scanning.exchange(true)) return;
    static TransformCand cands[64];
    static int ncands = 0;
    static int stage = 0;
    static uint64_t stageT0 = 0;
    uint64_t now = GetTickCount64();
    bool featureOn = g_fly_enabled.load() || g_noclip_enabled.load() || g_speed_enabled.load()
        || g_orbit_enabled.load() || g_size_enabled.load();
    if (!featureOn || now - g_self_tick.load() > 3000) { g_scanning.store(false); return; }

    if (stage == 0) {
        ncands = ScanForTransformCandidates(cands, 64, false);
        if (ncands == 0) ncands = ScanForTransformCandidates(cands, 64, true);
        g_scan_found.store(ncands);
        char dbg[96];
        snprintf(dbg, sizeof(dbg), "[Durag] Scan pass: %d transform candidates", ncands);
        OutputDebugStringA(dbg);
        if (ncands == 0) { g_scanning.store(false); return; }
        stage = 1;
        stageT0 = now;
        g_scanning.store(false);
        return;
    }
    if (stage == 1 && now - stageT0 >= 400) {
        float cx = g_self_x.load(), cy = g_self_y.load(), cz = g_self_z.load();
        uintptr_t winner = 0;
        for (int i = 0; i < ncands; i++) {
            float p[3];
            if (!safe_read_floats(cands[i].addr, p, 3)) continue;
            if (std::abs(p[0] - cx) <= 1.5f && std::abs(p[1] - cy) <= 1.5f && std::abs(p[2] - cz) <= 1.5f) { winner = cands[i].addr; break; }
        }
        if (winner) {
            g_player_locked.store(true);
            g_player_base.store(winner, std::memory_order_relaxed);
            g_base_fail_count.store(0);
            char dbg[96];
            snprintf(dbg, sizeof(dbg), "[Durag] Player transform located via scan: 0x%llx", (unsigned long long)winner);
            OutputDebugStringA(dbg);
            stage = 0;
        } else {
            stage = 0;
        }
    }
    g_scanning.store(false);
}

// Verify a staged HWBP candidate against the network-reported self position
// before trusting it. Wrong-site captures (e.g. render/scale structs) get
// discarded so they can never block the memory-scan fallback.
static void ValidateHwbpCandidate() {
    if (g_player_locked.load() || g_manual_base.load()) return;
    uintptr_t cand = g_hwbp_candidate.load(std::memory_order_relaxed);
    if (!cand) return;
    if (GetTickCount64() - g_self_tick.load() > 3000) return;   // no fresh reference yet
    cand += g_pos_offset.load(std::memory_order_relaxed);       // transform may live at +off inside the struct
    float p[3];
    if (!safe_read_floats(cand, p, 3)) {
        if (g_hwbp_cand_bad.fetch_add(1) + 1 >= 2) {
            OutputDebugStringA("[Durag] HWBP candidate unreadable - discarded");
            g_hwbp_candidate.store(0); g_hwbp_cand_ok.store(0); g_hwbp_cand_bad.store(0);
        }
        return;
    }
    float dx = p[0] - g_self_x.load(), dy = p[1] - g_self_y.load(), dz = p[2] - g_self_z.load();
    if (dx * dx + dy * dy + dz * dz <= 4.0f * 4.0f) {
        // Matches where the player actually is - require it twice for confidence
        if (g_hwbp_cand_ok.fetch_add(1) + 1 >= 2) {
            g_player_locked.store(true);
            g_player_base.store(cand, std::memory_order_relaxed);
            g_base_fail_count.store(0);
            g_hwbp_candidate.store(0); g_hwbp_cand_ok.store(0); g_hwbp_cand_bad.store(0);
            OutputDebugStringA("[Durag] Player base locked via HWBP (verified)");
        }
    } else {
        if (g_hwbp_cand_bad.fetch_add(1) + 1 >= 2) {
            OutputDebugStringA("[Durag] HWBP candidate rejected - not the transform");
            g_hwbp_candidate.store(0); g_hwbp_cand_ok.store(0); g_hwbp_cand_bad.store(0);
        }
    }
}

static void ValidateScanCapture() {
    if (!g_player_locked.load() || g_manual_base.load()) return;
    static int diverged = 0;
    if (g_step_hits.load() > 0) return;
    uintptr_t base = g_player_base.load(std::memory_order_relaxed);
    if (!base) return;
    float p[3];
    if (!safe_read_floats(base, p, 3)) { if (++diverged > 20) { diverged = 0; g_player_locked.store(false); g_player_base.store(0); OutputDebugStringA("[Durag] Scan capture lost (unreadable), rescanning"); } return; }
    float dx = p[0] - g_self_x.load(), dy = p[1] - g_self_y.load(), dz = p[2] - g_self_z.load();
    if (dx * dx + dy * dy + dz * dz > 75.0f * 75.0f) {
        if (++diverged > 40) { diverged = 0; g_player_locked.store(false); g_player_base.store(0); OutputDebugStringA("[Durag] Scan capture stale, rescanning"); }
    }
    else diverged = 0;
}

DWORD WINAPI HwbpWatchdogThread(LPVOID) {
    int wait_logs = 0;
    int quiet = 0;
    while (!g_shutdown.load()) {
        Sleep(250);
        if (g_player_locked.load()) { wait_logs = 0; ValidateScanCapture(); continue; }
        ValidateHwbpCandidate();
        uintptr_t addr = g_write_hook_addr;
        if (addr) {
            SetHardwareBreakpointOnAllThreads(addr, true);
            if (++quiet % 24 == 0) {
                char dbg[128];
                snprintf(dbg, sizeof(dbg), "[Durag] attach waiting: steps=%llu armed=%u",
                    (unsigned long long)g_step_hits.load(), g_hwbp_armed.load());
                OutputDebugStringA(dbg);
            }
            if (++wait_logs == 40) OutputDebugStringA("[Durag] No HWBP attach after 10s - falling back to memory scan");
            TryScanCapture();
        }
    }
    return 0;
}

// =============================================================================
// MOVEMENT
// =============================================================================

static bool key_down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

static double query_dt_seconds() {
    static LARGE_INTEGER freq = { 0 };
    static LARGE_INTEGER prev = { 0 };
    static bool init = false;
    if (!init) {
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&prev);
        init = true;
        return 0.001;
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double dt = (double)(now.QuadPart - prev.QuadPart) / (double)freq.QuadPart;
    prev = now;
    if (dt < 0.0001) dt = 0.0001;
    if (dt > 0.05) dt = 0.05;
    return dt;
}

static bool read_camera_quat(float* q) {
    uintptr_t cb = g_camera_base.load(std::memory_order_relaxed);
    if (!cb) return false;
    float tmp[4] = { 0, 0, 0, 1 };
    if (!safe_read_floats(cb + 0x0C, tmp, 4)) return false;
    if (!std::isfinite(tmp[0]) || !std::isfinite(tmp[1]) || !std::isfinite(tmp[2]) || !std::isfinite(tmp[3])) return false;
    float mag = tmp[0] * tmp[0] + tmp[1] * tmp[1] + tmp[2] * tmp[2] + tmp[3] * tmp[3];
    if (std::abs(mag - 1.0f) > 0.15f) return false;
    q[0] = tmp[0]; q[1] = tmp[1]; q[2] = tmp[2]; q[3] = tmp[3];
    return true;
}

void apply_movement() {
    uintptr_t base = g_player_base.load(std::memory_order_relaxed);
    if (!base) return;
    float pos[3];
    if (!safe_read_floats(base, pos, 3)) {
        int fails = g_base_fail_count.fetch_add(1) + 1;
        if (fails >= 50) {
            g_player_locked.store(false);
            g_player_base.store(0);
            g_base_fail_count.store(0);
            g_has_target.store(false);
            OutputDebugStringA("[Durag] Player base stale, re-attaching");
        }
        return;
    }
    g_base_fail_count.store(0);
    if (!std::isfinite(pos[0]) || !std::isfinite(pos[1]) || !std::isfinite(pos[2])) return;
    float x = pos[0], y = pos[1], z = pos[2];
    bool fly = g_fly_enabled.load(), noclip = g_noclip_enabled.load(), speed = g_speed_enabled.load(), orbit = g_orbit_enabled.load();
    bool size_on = g_size_enabled.load();
    bool goto_on = g_goto_enabled.load();
    bool slot_on = g_slot_enabled.load();
    bool active = fly || noclip || speed || orbit || size_on || goto_on || slot_on;
    if (!active) { g_has_target.store(false); return; }
    float tx, ty, tz;
    if (!g_has_target.exchange(true)) { tx = x; ty = y; tz = z; }
    else { tx = g_target_x.load(); ty = g_target_y.load(); tz = g_target_z.load(); if (!std::isfinite(tx) || !std::isfinite(ty) || !std::isfinite(tz)) { tx = x; ty = y; tz = z; } }
    float dt = (float)query_dt_seconds();

    if (slot_on) {
        uint64_t age = GetTickCount64() - g_slot_tick.load(std::memory_order_relaxed);
        if (age > 1000) age = 1000;
        float t = (float)age * 0.001f;
        float px = g_slot_x.load(std::memory_order_relaxed) + g_slot_vx.load(std::memory_order_relaxed) * t;
        float py = g_slot_y.load(std::memory_order_relaxed) + g_slot_vy.load(std::memory_order_relaxed) * t;
        float pz = g_slot_z.load(std::memory_order_relaxed) + g_slot_vz.load(std::memory_order_relaxed) * t;
        if (!std::isfinite(px) || !std::isfinite(py) || !std::isfinite(pz)) return;
        g_target_x.store(px); g_target_y.store(py); g_target_z.store(pz);
        safe_write_float(base, px); safe_write_float(base + 4, py); safe_write_float(base + 8, pz);
        if (size_on) apply_scale(base);
        return;
    }

    if (goto_on) {
        float gx = g_goto_x.load(), gy = g_goto_y.load(), gz = g_goto_z.load();
        float ddx = gx - x, ddy = gy - y, ddz = gz - z;
        float dist = std::sqrt(ddx * ddx + ddy * ddy + ddz * ddz);
        float stop = g_goto_stop.load();
        if (dist > stop) {
            float sp = g_goto_speed.load();
            float step = sp * dt;
            if (step > dist) step = dist;
            tx = x + ddx / dist * step;
            ty = y + ddy / dist * step;
            tz = z + ddz / dist * step;
            if (!std::isfinite(tx) || !std::isfinite(ty) || !std::isfinite(tz)) { tx = x; ty = y; tz = z; }
            g_target_x.store(tx); g_target_y.store(ty); g_target_z.store(tz);
            safe_write_float(base, tx); safe_write_float(base + 4, ty); safe_write_float(base + 8, tz);
            if (size_on) apply_scale(base);
            return;
        }
        g_has_target.store(false);
        return;
    }

    if (orbit) {
        float targetX = 0, targetY = 0, targetZ = 0; bool foundTarget = false;
        {
            std::lock_guard<std::mutex> lk(g_net_mutex);
            float nearestDist = 999999.0f; int targetIdx = g_orbit_target.load();
            if (targetIdx == -1) {
                for (int i = 0; i < g_net_entity_count.load(); i++) {
                    if (GetTickCount64() - g_net_entities[i].last_update > 10000 || g_net_entities[i].dead || !g_net_entities[i].has_pos) continue;
                    float dx = g_net_entities[i].x - x, dy = g_net_entities[i].y - y, dz = g_net_entities[i].z - z;
                    float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
                    if (dist > 1.0f && dist < nearestDist) { nearestDist = dist; targetX = g_net_entities[i].x; targetY = g_net_entities[i].y; targetZ = g_net_entities[i].z; foundTarget = true; }
                }
            }
            else if (targetIdx >= 0 && targetIdx < g_net_entity_count.load()) {
                if (GetTickCount64() - g_net_entities[targetIdx].last_update < 10000 && !g_net_entities[targetIdx].dead && g_net_entities[targetIdx].has_pos) {
                    targetX = g_net_entities[targetIdx].x; targetY = g_net_entities[targetIdx].y; targetZ = g_net_entities[targetIdx].z; foundTarget = true;
                }
            }
        }
        if (foundTarget) {
            g_orbit_angle += g_orbit_speed.load() * dt;
            if (g_orbit_angle > 6.28318530) g_orbit_angle -= 6.28318530;
            float ang = (float)(g_orbit_angle + g_orbit_phase.load());
            tx = targetX + std::cosf(ang) * g_orbit_radius.load();
            tz = targetZ + std::sinf(ang) * g_orbit_radius.load();
            ty = targetY + 1.0f;
            if (!std::isfinite(tx) || !std::isfinite(ty) || !std::isfinite(tz)) { tx = x; ty = y; tz = z; }
            g_target_x.store(tx); g_target_y.store(ty); g_target_z.store(tz);
            safe_write_float(base, tx); safe_write_float(base + 4, ty); safe_write_float(base + 8, tz);
            if (size_on) apply_scale(base);
            return;
        }
    }

    float quat[4] = { 0, 0, 0, 1 };
    bool hasValidQuat = read_camera_quat(quat);
    float qx = quat[0], qy = quat[1], qz = quat[2], qw = quat[3];
    float forwardY = hasValidQuat ? 2.0f * (qw * qx - qy * qz) : 0.0f;
    float forwardX, forwardZ, rightX, rightZ;
    if (hasValidQuat) {
        forwardX = 2.0f * (qx * qz + qw * qy); forwardZ = 1.0f - 2.0f * (qx * qx + qy * qy);
        float fLen = std::sqrt(forwardX * forwardX + forwardZ * forwardZ);
        if (fLen > 0.001f) { forwardX = -(forwardX / fLen); forwardZ = -(forwardZ / fLen); }
        else { forwardX = 0; forwardZ = -1; }
        rightX = 1.0f - 2.0f * (qy * qy + qz * qz); rightZ = 2.0f * (qx * qz - qw * qy);
        float rLen = std::sqrt(rightX * rightX + rightZ * rightZ);
        if (rLen > 0.001f) { rightX /= rLen; rightZ /= rLen; }
        else { rightX = 1; rightZ = 0; }
    }
    else { forwardX = 0; forwardZ = -1; rightX = 1; rightZ = 0; }
    float dx = 0, dz = 0;
    if (key_down('W')) { dx += forwardX; dz += forwardZ; }
    if (key_down('S')) { dx -= forwardX; dz -= forwardZ; }
    if (key_down('D')) { dx += rightX; dz += rightZ; }
    if (key_down('A')) { dx -= rightX; dz -= rightZ; }
    float len = std::sqrt(dx * dx + dz * dz);
    if (len > 0.001f) { dx /= len; dz /= len; }
    if (fly || noclip) {
        float flySpeed = g_fly_speed.load();
        bool view = g_view_fly.load() && hasValidQuat;
        if (view) {
            float fAmt = (key_down('W') ? 1.0f : 0.0f) - (key_down('S') ? 1.0f : 0.0f);
            float rAmt = (key_down('D') ? 1.0f : 0.0f) - (key_down('A') ? 1.0f : 0.0f);
            tx += (forwardX * fAmt + rightX * rAmt) * flySpeed * dt;
            tz += (forwardZ * fAmt + rightZ * rAmt) * flySpeed * dt;
            ty += forwardY * fAmt * flySpeed * dt;
        }
        else {
            tx += dx * flySpeed * dt; tz += dz * flySpeed * dt;
        }
        if (key_down(VK_SPACE)) ty += flySpeed * dt;
        if (key_down(VK_SHIFT)) ty -= flySpeed * dt;
    }
    else if (speed) {
        float mult = g_speed_mult.load();
        tx += dx * 5.0f * mult * dt; tz += dz * 5.0f * mult * dt; ty = y;
    }
    if (!std::isfinite(tx) || !std::isfinite(ty) || !std::isfinite(tz)) { tx = x; ty = y; tz = z; }
    g_target_x.store(tx); g_target_y.store(ty); g_target_z.store(tz);
    safe_write_float(base, tx); safe_write_float(base + 4, ty); safe_write_float(base + 8, tz);
    if (size_on) apply_scale(base);
}

DWORD WINAPI MovementSpamThread(LPVOID) {
    while (g_movement_thread_running.load()) { apply_movement(); Sleep(1); }
    return 0;
}

void executor_tick() {}

// =============================================================================
// DXGI PRESENT
// =============================================================================

HRESULT __stdcall hooked_present(IDXGISwapChain* swapchain, UINT sync_interval, UINT flags) {
    HRESULT hr = g_original_present(swapchain, sync_interval, flags);
    static std::atomic<bool> ticking{ false };
    bool expected = false;
    if (ticking.compare_exchange_strong(expected, true)) { executor_tick(); ticking.store(false); }
    return hr;
}

bool hook_dxgi_present() {
    DXGI_SWAP_CHAIN_DESC desc = {};
    desc.BufferCount = 2; desc.BufferDesc.Width = 1; desc.BufferDesc.Height = 1;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.BufferDesc.RefreshRate = { 60, 1 };
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.OutputWindow = GetDesktopWindow();
    desc.SampleDesc.Count = 1; desc.Windowed = TRUE; desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    ID3D11Device* device = nullptr; IDXGISwapChain* swapchain = nullptr;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &desc, &swapchain, &device, nullptr, nullptr);
    if (FAILED(hr) || !swapchain) hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &desc, &swapchain, &device, nullptr, nullptr);
    if (FAILED(hr) || !swapchain) return false;
    void** vtable = *reinterpret_cast<void***>(swapchain);
    g_present_addr = (uintptr_t)vtable[8];
    memcpy(g_swap_chain_vtable, vtable, sizeof(g_swap_chain_vtable));
    swapchain->Release(); if (device) device->Release();
    if (MH_Initialize() != MH_OK && MH_Initialize() != MH_ERROR_ALREADY_INITIALIZED) return false;
    if (MH_CreateHook((LPVOID)g_present_addr, reinterpret_cast<LPVOID>(&hooked_present), reinterpret_cast<LPVOID*>(&g_original_present)) != MH_OK) return false;
    if (MH_EnableHook((LPVOID)g_present_addr) != MH_OK) return false;
    OutputDebugStringA("[Durag] DXGI Present hooked");
    return true;
}

// =============================================================================
// SCRIPT HOST API
// =============================================================================

static bool api_get_self_pos(float* x, float* y, float* z) {
    uintptr_t base = g_player_base.load(std::memory_order_relaxed);
    if (!base) return false;
    float p[3];
    if (!safe_read_floats(base, p, 3)) return false;
    *x = p[0]; *y = p[1]; *z = p[2];
    return true;
}

static bool api_set_self_pos(float x, float y, float z) {
    uintptr_t base = g_player_base.load(std::memory_order_relaxed);
    if (!base) return false;
    bool ok = safe_write_float(base, x);
    ok &= safe_write_float(base + 4, y);
    ok &= safe_write_float(base + 8, z);
    return ok;
}

static float api_get_self_yaw() { return g_self_yaw.load(std::memory_order_relaxed); }

static void api_set_flag(const char* name, bool on) {
    if (!strcmp(name, "fly")) g_fly_enabled.store(on);
    else if (!strcmp(name, "noclip")) g_noclip_enabled.store(on);
    else if (!strcmp(name, "speed")) g_speed_enabled.store(on);
    else if (!strcmp(name, "size")) g_size_enabled.store(on);
}

static bool api_get_flag(const char* name) {
    if (!strcmp(name, "fly")) return g_fly_enabled.load();
    if (!strcmp(name, "noclip")) return g_noclip_enabled.load();
    if (!strcmp(name, "speed")) return g_speed_enabled.load();
    if (!strcmp(name, "size")) return g_size_enabled.load();
    return false;
}

static double api_get_mult(const char* name) {
    if (!strcmp(name, "fly_speed")) return g_fly_speed.load();
    if (!strcmp(name, "speed_mult")) return g_speed_mult.load();
    if (!strcmp(name, "size_scale")) return g_size.load();
    if (!strcmp(name, "orbit_radius")) return g_orbit_radius.load();
    if (!strcmp(name, "orbit_speed")) return g_orbit_speed.load();
    return 0.0;
}

static void api_set_mult(const char* name, double v) {
    if (!strcmp(name, "fly_speed")) g_fly_speed.store(v);
    else if (!strcmp(name, "speed_mult")) g_speed_mult.store(v);
    else if (!strcmp(name, "size_scale")) g_size.store(v);
    else if (!strcmp(name, "orbit_radius")) g_orbit_radius.store(v);
    else if (!strcmp(name, "orbit_speed")) g_orbit_speed.store(v);
}

static void api_stop_all() {
    g_fly_enabled.store(false);
    g_noclip_enabled.store(false);
    g_speed_enabled.store(false);
    g_orbit_enabled.store(false);
    g_size_enabled.store(false);
    g_goto_enabled.store(false);
    g_slot_enabled.store(false);
}

static uint32_t api_self_pid() { return GetCurrentProcessId(); }

// Discover every injected client: named pipes are durag_executor_<pid>
static int api_list_client_pids(uint32_t* out, int max) {
    if (!out || max <= 0) return 0;
    int n = 0;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA("\\\\.\\pipe\\durag_executor_*", &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        const char* nm = fd.cFileName;
        const char* p = strstr(nm, "durag_executor_");
        if (!p) continue;
        unsigned long pid = strtoul(p + 15, nullptr, 10);
        if (!pid) continue;
        // dedupe
        int dup = 0;
        for (int i = 0; i < n; i++) if (out[i] == pid) { dup = 1; break; }
        if (!dup && n < max) out[n++] = (uint32_t)pid;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    // sort ascending
    for (int i = 1; i < n; i++) {
        uint32_t v = out[i];
        int j = i - 1;
        while (j >= 0 && out[j] > v) { out[j + 1] = out[j]; j--; }
        out[j + 1] = v;
    }
    return n;
}

DuragHostApi g_host = {
    api_get_self_pos,
    api_set_self_pos,
    api_get_self_yaw,
    api_set_flag,
    api_get_flag,
    api_get_mult,
    api_set_mult,
    api_stop_all,
    api_self_pid,
    api_list_client_pids,
};

// =============================================================================
// IPC
// =============================================================================

static bool send_game_frame(const uint8_t* data, size_t len) {
    SOCKET s = g_game_socket.load();
    if (s == INVALID_SOCKET || !data || len == 0 || len > 4096) return false;
    int sent = send(s, (const char*)data, (int)len, 0);
    return sent == (int)len;
}

// Extract an escaped string field from the flat JSON commands the UI sends.
static bool ExtractJsonStringField(const std::string& json, const char* field, std::string& out) {
    std::string needle = "\"" + std::string(field) + "\":";
    size_t p = json.find(needle);
    if (p == std::string::npos) return false;
    p += needle.size();
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) p++;
    if (p >= json.size() || json[p] != '"') return false;
    p++;
    out.clear();
    while (p < json.size()) {
        char c = json[p++];
        if (c == '"') return true;
        if (c == '\\' && p < json.size()) {
            char e = json[p++];
            switch (e) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'n':  out += '\n'; break;
                case 't':  out += '\t'; break;
                case 'r':  out += '\r'; break;
                case 'b':  out += '\b'; break;
                case 'f':  out += '\f'; break;
                default:   out += e;    break;
            }
        } else out += c;
    }
    return false;
}

std::string handle_command(const std::string& cmd) {
    std::string response;
    if (cmd.find("\"cmd\":\"ping\"") != std::string::npos) { response = "{\"status\":\"ok\",\"msg\":\"pong\"}"; }
    else if (cmd.find("\"cmd\":\"world\"") != std::string::npos || cmd.find("\"cmd\":\"player\"") != std::string::npos) {
        uintptr_t b = g_player_base.load(std::memory_order_relaxed);
        float x = 0, y = 0, z = 0;
        if (b) { float pos[3]; if (safe_read_floats(b, pos, 3)) { x = pos[0]; y = pos[1]; z = pos[2]; } }
        char buf[128]; snprintf(buf, sizeof(buf), "{\"status\":\"ok\",\"world\":\"0x%llx\",\"x\":%.2f,\"y\":%.2f,\"z\":%.2f}", b, x, y, z);
        response = buf;
    }
    else if (cmd.find("\"cmd\":\"attach\"") != std::string::npos) { g_player_locked.store(false); g_player_base.store(0); response = "{\"status\":\"ok\",\"msg\":\"re-attaching... move in game\"}"; }
    else if (cmd.find("\"cmd\":\"list_net_entities\"") != std::string::npos) {
        std::lock_guard<std::mutex> lk(g_net_mutex);
        uintptr_t playerBase = g_player_base.load();
        float px = 0, py = 0, pz = 0;
        if (playerBase) { float pos[3]; if (safe_read_floats(playerBase, pos, 3)) { px = pos[0]; py = pos[1]; pz = pos[2]; } }
        uint64_t now = GetTickCount64();
        std::string json = "{\"status\":\"ok\",\"count\":" + std::to_string(g_net_entity_count.load()) + ",\"entities\":[";
        bool first = true;
        for (int i = 0; i < g_net_entity_count.load(); i++) {
            if (now - g_net_entities[i].last_update > 10000 || !g_net_entities[i].has_pos) continue;
            float dx = g_net_entities[i].x - px, dy = g_net_entities[i].y - py, dz = g_net_entities[i].z - pz;
            float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (!first) json += ","; first = false;
            char buf[400];
            snprintf(buf, sizeof(buf),
                "{\"idx\":%d,\"id\":%llu,\"name\":\"%s\",\"x\":%.1f,\"y\":%.1f,\"z\":%.1f,\"dist\":%.1f,\"moving\":%s,\"dead\":%s}",
                i, (unsigned long long)g_net_entities[i].id,
                g_net_entities[i].name[0] ? g_net_entities[i].name : "",
                g_net_entities[i].x, g_net_entities[i].y, g_net_entities[i].z, dist,
                g_net_entities[i].moving ? "true" : "false",
                g_net_entities[i].dead ? "true" : "false");
            json += buf;
        }
        json += "]}"; response = json;
    }
    else if (cmd.find("\"cmd\":\"teleport\"") != std::string::npos) {
        double tx = 0, ty = 0, tz = 0;
        if (sscanf_s(cmd.c_str(), "{\"cmd\":\"teleport\",\"x\":%lf,\"y\":%lf,\"z\":%lf}", &tx, &ty, &tz) == 3) {
            uintptr_t base = g_player_base.load();
            if (base) { safe_write_float(base, (float)tx); safe_write_float(base + 4, (float)ty); safe_write_float(base + 8, (float)tz); g_target_x.store((float)tx); g_target_y.store((float)ty); g_target_z.store((float)tz); response = "{\"status\":\"ok\",\"msg\":\"teleported\"}"; }
            else response = "{\"status\":\"error\",\"msg\":\"no player base\"}";
        }
        else response = "{\"status\":\"error\",\"msg\":\"invalid coords\"}";
    }
    else if (cmd.find("\"cmd\":\"teleport_net\"") != std::string::npos) {
        int idx = -1; sscanf_s(cmd.c_str(), "{\"cmd\":\"teleport_net\",\"idx\":%d}", &idx);
        uintptr_t base = g_player_base.load();
        if (!base) response = "{\"status\":\"error\",\"msg\":\"no player base\"}";
        else {
            std::lock_guard<std::mutex> lk(g_net_mutex); uint64_t now = GetTickCount64(); float tx = 0, ty = 0, tz = 0; bool found = false;
            if (idx == -1) {
                float px = 0, py = 0, pz = 0; float pos[3]; if (safe_read_floats(base, pos, 3)) { px = pos[0]; py = pos[1]; pz = pos[2]; }
                float nearestDist = 999999.0f;
                for (int i = 0; i < g_net_entity_count.load(); i++) {
                    if (now - g_net_entities[i].last_update > 10000 || g_net_entities[i].dead || !g_net_entities[i].has_pos) continue;
                    float dx = g_net_entities[i].x - px, dy = g_net_entities[i].y - py, dz = g_net_entities[i].z - pz; float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
                    if (dist > 1.0f && dist < nearestDist) { nearestDist = dist; tx = g_net_entities[i].x; ty = g_net_entities[i].y; tz = g_net_entities[i].z; found = true; }
                }
            }
            else if (idx >= 0 && idx < g_net_entity_count.load()) { if (now - g_net_entities[idx].last_update < 10000 && g_net_entities[idx].has_pos) { tx = g_net_entities[idx].x; ty = g_net_entities[idx].y; tz = g_net_entities[idx].z; found = true; } }
            if (found) { safe_write_float(base, tx); safe_write_float(base + 4, ty); safe_write_float(base + 8, tz); g_target_x.store(tx); g_target_y.store(ty); g_target_z.store(tz); response = "{\"status\":\"ok\",\"msg\":\"teleported\"}"; }
            else response = "{\"status\":\"error\",\"msg\":\"no entities found\"}";
        }
    }
    else if (cmd.find("\"cmd\":\"set_orbit\"") != std::string::npos) { g_orbit_enabled.store(cmd.find("true") != std::string::npos); response = "{\"status\":\"ok\"}"; }
    else if (cmd.find("\"cmd\":\"set_orbit_speed\"") != std::string::npos) { float val = 2.0f; sscanf_s(cmd.c_str(), "{\"cmd\":\"set_orbit_speed\",\"val\":%f}", &val); g_orbit_speed.store(val); response = "{\"status\":\"ok\"}"; }
    else if (cmd.find("\"cmd\":\"set_orbit_radius\"") != std::string::npos) { float val = 5.0f; sscanf_s(cmd.c_str(), "{\"cmd\":\"set_orbit_radius\",\"val\":%f}", &val); g_orbit_radius.store(val); response = "{\"status\":\"ok\"}"; }
    else if (cmd.find("\"cmd\":\"set_orbit_phase\"") != std::string::npos) { float val = 0.0f; sscanf_s(cmd.c_str(), "{\"cmd\":\"set_orbit_phase\",\"val\":%f}", &val); g_orbit_phase.store(val); response = "{\"status\":\"ok\"}"; }
    else if (cmd.find("\"cmd\":\"set_slot\"") != std::string::npos) {
        float sx = 0, sy = 0, sz = 0, vx = 0, vy = 0, vz = 0;
        int n = sscanf_s(cmd.c_str(), "{\"cmd\":\"set_slot\",\"x\":%f,\"y\":%f,\"z\":%f,\"vx\":%f,\"vy\":%f,\"vz\":%f}", &sx, &sy, &sz, &vx, &vy, &vz);
        if (n >= 3) {
            g_slot_x.store(sx); g_slot_y.store(sy); g_slot_z.store(sz);
            if (n >= 6) { g_slot_vx.store(vx); g_slot_vy.store(vy); g_slot_vz.store(vz); }
            else { g_slot_vx.store(0); g_slot_vy.store(0); g_slot_vz.store(0); }
            g_slot_tick.store(GetTickCount64(), std::memory_order_relaxed);
            g_slot_enabled.store(true);
            response = "{\"status\":\"ok\",\"msg\":\"slot set\"}";
        }
        else response = "{\"status\":\"error\",\"msg\":\"bad slot\"}";
    }
    else if (cmd.find("\"cmd\":\"set_goto_off\"") != std::string::npos) { g_goto_enabled.store(false); g_slot_enabled.store(false); response = "{\"status\":\"ok\",\"msg\":\"goto off\"}"; }
    else if (cmd.find("\"cmd\":\"set_goto\"") != std::string::npos) {
        float gx = 0, gy = 0, gz = 0, sp = 6.0f, stp = 1.0f;
        int n = sscanf_s(cmd.c_str(), "{\"cmd\":\"set_goto\",\"x\":%f,\"y\":%f,\"z\":%f,\"speed\":%f,\"stop\":%f}", &gx, &gy, &gz, &sp, &stp);
        if (n >= 3) {
            g_goto_x.store(gx); g_goto_y.store(gy); g_goto_z.store(gz);
            if (n >= 4 && sp > 0.1f && sp < 200.0f) g_goto_speed.store(sp);
            if (n >= 5 && stp >= 0.5f && stp < 200.0f) g_goto_stop.store(stp);
            g_goto_enabled.store(true);
            response = "{\"status\":\"ok\",\"msg\":\"goto set\"}";
        }
        else response = "{\"status\":\"error\",\"msg\":\"bad goto\"}";
    }
    else if (cmd.find("\"cmd\":\"set_orbit_target\"") != std::string::npos) {
        uint64_t targetId = 0; sscanf_s(cmd.c_str(), "{\"cmd\":\"set_orbit_target\",\"id\":%llu}", &targetId);
        std::lock_guard<std::mutex> lk(g_net_mutex); int foundIdx = -1;
        for (int i = 0; i < g_net_entity_count.load(); i++) { if (g_net_entities[i].id == targetId) { foundIdx = i; break; } }
        if (foundIdx >= 0) { g_orbit_target.store(foundIdx); response = "{\"status\":\"ok\"}"; }
        else response = "{\"status\":\"error\",\"msg\":\"entity not found\"}";
    }
    else if (cmd.find("\"cmd\":\"set_fly\"") != std::string::npos) { g_fly_enabled.store(cmd.find("true") != std::string::npos); response = "{\"status\":\"ok\"}"; }
    else if (cmd.find("\"cmd\":\"set_noclip\"") != std::string::npos) { g_noclip_enabled.store(cmd.find("true") != std::string::npos); response = "{\"status\":\"ok\"}"; }
    else if (cmd.find("\"cmd\":\"set_speed\"") != std::string::npos) { g_speed_enabled.store(cmd.find("true") != std::string::npos); response = "{\"status\":\"ok\"}"; }
    else if (cmd.find("\"cmd\":\"set_fly_speed\"") != std::string::npos) { float val = 50.0f; sscanf_s(cmd.c_str(), "{\"cmd\":\"set_fly_speed\",\"val\":%f}", &val); g_fly_speed.store(val); response = "{\"status\":\"ok\"}"; }
    else if (cmd.find("\"cmd\":\"set_speed_mult\"") != std::string::npos) { float val = 2.5f; sscanf_s(cmd.c_str(), "{\"cmd\":\"set_speed_mult\",\"val\":%f}", &val); g_speed_mult.store(val); response = "{\"status\":\"ok\"}"; }
    else if (cmd.find("\"cmd\":\"set_size\"") != std::string::npos) { g_size_enabled.store(cmd.find("true") != std::string::npos); response = "{\"status\":\"ok\"}"; }
    else if (cmd.find("\"cmd\":\"set_size_val\"") != std::string::npos) {
        float val = 1.0f;
        sscanf_s(cmd.c_str(), "{\"cmd\":\"set_size_val\",\"val\":%f}", &val);
        if (!std::isfinite(val)) val = 1.0f;
        if (val < 0.05f) val = 0.05f;
        if (val > 100.0f) val = 100.0f;
        g_size.store(val);
        response = "{\"status\":\"ok\"}";
    }
    else if (cmd.find("\"cmd\":\"set_player_base\"") != std::string::npos) {
        uintptr_t addr = 0;
        if (sscanf_s(cmd.c_str(), "{\"cmd\":\"set_player_base\",\"addr\":\"%llx\"", &addr) != 1)
            sscanf_s(cmd.c_str(), "{\"cmd\":\"set_player_base\",\"addr\":%llx", &addr);
        if (addr) {
            g_player_base.store(addr, std::memory_order_relaxed);
            g_player_locked.store(true);
            g_base_fail_count.store(0);
            g_manual_base.store(true);
            char b[96];
            snprintf(b, sizeof(b), "{\"status\":\"ok\",\"msg\":\"player base set 0x%llx\"}", (unsigned long long)addr);
            response = b;
        }
        else response = "{\"status\":\"error\",\"msg\":\"invalid addr\"}";
    }
    else if (cmd.find("\"cmd\":\"clear_player_base\"") != std::string::npos) {
        g_manual_base.store(false);
        g_player_locked.store(false);
        g_player_base.store(0);
        response = "{\"status\":\"ok\",\"msg\":\"player base cleared\"}";
    }
    else if (cmd.find("\"cmd\":\"dump_addr\"") != std::string::npos) {
        uintptr_t addr = 0;
        int count = 16;
        if (sscanf_s(cmd.c_str(), "{\"cmd\":\"dump_addr\",\"addr\":\"%llx\"", &addr) != 1)
            sscanf_s(cmd.c_str(), "{\"cmd\":\"dump_addr\",\"addr\":%llx", &addr);
        sscanf_s(cmd.c_str(), "{\"cmd\":\"dump_addr\",\"addr\":\"%*llx\",\"count\":%d", &count) == 1 ||
            sscanf_s(cmd.c_str(), "{\"cmd\":\"dump_addr\",\"addr\":%*llx,\"count\":%d", &count);
        if (addr && count >= 1 && count <= 64) {
            float vals[64] = { 0 };
            int got = 0;
            for (int i = 0; i < count; i++) {
                float v = 0;
                if (!safe_read_floats(addr + (uintptr_t)i * 4, &v, 1)) break;
                vals[i] = v;
                got++;
            }
            char hdr[80], fbuf[24];
            snprintf(hdr, sizeof(hdr), "{\"status\":\"ok\",\"addr\":\"0x%llx\",\"read\":%d,\"floats\":[", (unsigned long long)addr, got);
            std::string json(hdr);
            for (int i = 0; i < got; i++) { snprintf(fbuf, sizeof(fbuf), "%s%.4g", i ? "," : "", vals[i]); json += fbuf; }
            json += "]}";
            response = json;
        }
        else response = "{\"status\":\"error\",\"msg\":\"bad addr/count\"}";
    }
    else if (cmd.find("\"cmd\":\"status\"") != std::string::npos) {
        char b[512];
        const char* src = g_manual_base.load() ? "manual" : (g_step_hits.load() > 0 ? "hwbp" : "scan");
        snprintf(b, sizeof(b),
            "{\"status\":\"ok\",\"locked\":%s,\"src\":\"%s\",\"base\":\"0x%llx\",\"cam\":\"0x%llx\",\"sock\":%llu,\"ents\":%d,\"veh\":%s,\"hwbp_addr\":\"0x%llx\",\"threads\":%u,\"armed\":%u,\"armfail\":%u,\"steps\":%llu,\"self\":[%.2f,%.2f,%.2f,%.3f],\"size\":%.2f,\"size_on\":%s,\"fly\":%s,\"noclip\":%s,\"speed\":%s,\"orbit\":%s}",
            g_player_locked.load() ? "true" : "false",
            src,
            (unsigned long long)g_player_base.load(),
            (unsigned long long)g_camera_base.load(),
            (unsigned long long)g_game_socket.load(),
            g_net_entity_count.load(),
            g_veh_installed.load() ? "true" : "false",
            (unsigned long long)g_write_hook_addr,
            g_hwbp_threads.load(), g_hwbp_armed.load(), g_hwbp_failed.load(),
            (unsigned long long)g_step_hits.load(),
            g_self_x.load(), g_self_y.load(), g_self_z.load(), g_self_yaw.load(),
            g_size.load(),
            g_size_enabled.load() ? "true" : "false",
            g_fly_enabled.load() ? "true" : "false",
            g_noclip_enabled.load() ? "true" : "false",
            g_speed_enabled.load() ? "true" : "false",
            g_orbit_enabled.load() ? "true" : "false");
        response = b;
    }
    else if (cmd.find("\"cmd\":\"set_camera_base\"") != std::string::npos) {
        uintptr_t addr = 0;
        if (sscanf_s(cmd.c_str(), "{\"cmd\":\"set_camera_base\",\"addr\":\"%llx\"", &addr) != 1)
            sscanf_s(cmd.c_str(), "{\"cmd\":\"set_camera_base\",\"addr\":%llx", &addr);
        if (addr) { g_camera_base.store(addr); response = "{\"status\":\"ok\",\"msg\":\"camera base set\"}"; }
        else response = "{\"status\":\"error\",\"msg\":\"invalid addr\"}";
    }
    else if (cmd.find("\"cmd\":\"clear_camera_base\"") != std::string::npos) {
        g_camera_base.store(0);
        g_view_fly.store(false);
        response = "{\"status\":\"ok\",\"msg\":\"camera base cleared\"}";
    }
    else if (cmd.find("\"cmd\":\"set_view_fly\"") != std::string::npos) { g_view_fly.store(cmd.find("true") != std::string::npos); response = "{\"status\":\"ok\"}"; }
    else if (cmd.find("\"cmd\":\"camera\"") != std::string::npos) {
        uintptr_t cb = g_camera_base.load();
        float q[4] = { 0, 0, 0, 1 };
        bool valid = cb ? read_camera_quat(q) : false;
        float fx = 0, fy = 0, fz = -1;
        if (valid) {
            fx = 2.0f * (q[0] * q[2] + q[3] * q[1]); fz = 1.0f - 2.0f * (q[0] * q[0] + q[1] * q[1]);
            float l = std::sqrt(fx * fx + fz * fz);
            if (l > 0.001f) { fx = -(fx / l); fz = -(fz / l); } else { fx = 0; fz = -1; }
            fy = 2.0f * (q[3] * q[0] - q[1] * q[2]);
        }
        char buf[192];
        snprintf(buf, sizeof(buf), "{\"status\":\"ok\",\"cam\":\"0x%llx\",\"valid\":%s,\"fx\":%.3f,\"fy\":%.3f,\"fz\":%.3f,\"view_fly\":%s}",
            (unsigned long long)cb, valid ? "true" : "false", fx, fy, fz, g_view_fly.load() ? "true" : "false");
        response = buf;
    }
    else if (cmd.find("\"cmd\":\"packet_log\"") != std::string::npos) { g_packet_log_enabled.store(cmd.find("true") != std::string::npos); response = "{\"status\":\"ok\"}"; }
    else if (cmd.find("\"cmd\":\"get_packets\"") != std::string::npos) {
        std::lock_guard<std::mutex> lk(g_packet_log_mutex);
        std::string json = "{\"status\":\"ok\",\"count\":" + std::to_string(g_packet_log_entries.size()) + ",\"packets\":[";
        bool first = true;
        char pbuf[192];
        for (const auto& p : g_packet_log_entries) {
            snprintf(pbuf, sizeof(pbuf), "{\"t\":%llu,\"op\":%u,\"len\":%u,\"dir\":\"%s\",\"d\":\"%s\"}", (unsigned long long)p.tick, (unsigned)p.op, p.len, p.dir ? "out" : "in", p.desc);
            if (!first) json += ",";
            first = false;
            json += pbuf;
        }
        json += "]}";
        g_packet_log_entries.clear();
        response = json;
    }
    else if (cmd.find("\"cmd\":\"send_frame\"") != std::string::npos) {
        char hex[4096] = { 0 };
        if (sscanf_s(cmd.c_str(), "{\"cmd\":\"send_frame\",\"hex\":\"%4095[^\\\"]\"", hex, (unsigned)sizeof(hex)) == 1) {
            size_t hlen = strlen(hex);
            if (hlen >= 2 && hlen % 2 == 0 && hlen <= 4096) {
                uint8_t frame[2048];
                size_t flen = 0;
                bool ok = true;
                auto nyb = [](char c) -> int {
                    if (c >= '0' && c <= '9') return c - '0';
                    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                    return -1;
                };
                for (size_t i = 0; i + 1 < hlen && flen < sizeof(frame); i += 2) {
                    int hi = nyb(hex[i]), lo = nyb(hex[i + 1]);
                    if (hi < 0 || lo < 0) { ok = false; break; }
                    frame[flen++] = (uint8_t)((hi << 4) | lo);
                }
                if (ok && flen > 0) {
                    bool sent = send_game_frame(frame, flen);
                    char rb[96];
                    snprintf(rb, sizeof(rb), "{\"status\":\"%s\",\"sent\":%zu}", sent ? "ok" : "error", flen);
                    response = rb;
                }
                else response = "{\"status\":\"error\",\"msg\":\"bad hex\"}";
            }
            else response = "{\"status\":\"error\",\"msg\":\"bad hex\"}";
        }
        else response = "{\"status\":\"error\",\"msg\":\"missing hex\"}";
    }
    else if (cmd.find("\"cmd\":\"dump_player\"") != std::string::npos) {
        uintptr_t b = g_player_base.load();
        if (!b) { response = "{\"status\":\"error\",\"msg\":\"no player base\"}"; }
        else {
            float vals[32] = { 0 };
            int got = 0;
            for (int i = 0; i < 8; i++) {
                float chunk[4];
                if (!safe_read_floats(b + (uintptr_t)i * 16, chunk, 4)) break;
                for (int j = 0; j < 4; j++) vals[i * 4 + j] = chunk[j];
                got++;
            }
            char hdr[72], fbuf[24];
            snprintf(hdr, sizeof(hdr), "{\"status\":\"ok\",\"base\":\"0x%llx\",\"read\":%d,\"floats\":[", (unsigned long long)b, got * 4);
            std::string json(hdr);
            for (int i = 0; i < got * 4; i++) { snprintf(fbuf, sizeof(fbuf), "%s%.3g", i ? "," : "", vals[i]); json += fbuf; }
            json += "]}";
            response = json;
        }
    }
    else if (cmd.find("\"cmd\":\"exec_script\"") != std::string::npos) {
        // Legacy pipe-internal shortcuts (squad stop, old UI sends)
        if (cmd.find("--stop") != std::string::npos) {
            api_stop_all();
            response = "{\"status\":\"ok\",\"msg\":\"stopped\"}";
        }
        else {
            std::string code;
            if (!ExtractJsonStringField(cmd, "code", code)) {
                // keep the old substring toggles working for senders without a code field
                if (cmd.find("fly on") != std::string::npos) { api_set_flag("fly", true); response = "{\"status\":\"ok\",\"msg\":\"fly on\"}"; }
                else if (cmd.find("fly off") != std::string::npos) { api_set_flag("fly", false); response = "{\"status\":\"ok\",\"msg\":\"fly off\"}"; }
                else if (cmd.find("noclip on") != std::string::npos) { api_set_flag("noclip", true); response = "{\"status\":\"ok\",\"msg\":\"noclip on\"}"; }
                else if (cmd.find("noclip off") != std::string::npos) { api_set_flag("noclip", false); response = "{\"status\":\"ok\",\"msg\":\"noclip off\"}"; }
                else if (cmd.find("speed on") != std::string::npos) { api_set_flag("speed", true); response = "{\"status\":\"ok\",\"msg\":\"speed on\"}"; }
                else if (cmd.find("speed off") != std::string::npos) { api_set_flag("speed", false); response = "{\"status\":\"ok\",\"msg\":\"speed off\"}"; }
                else if (cmd.find("size on") != std::string::npos) { api_set_flag("size", true); response = "{\"status\":\"ok\",\"msg\":\"size on\"}"; }
                else if (cmd.find("size off") != std::string::npos) { api_set_flag("size", false); response = "{\"status\":\"ok\",\"msg\":\"size off\"}"; }
                else response = "{\"status\":\"error\",\"msg\":\"no code field\"}";
            } else {
                response = Script_Exec(code);
            }
        }
    }
    else if (cmd.find("\"cmd\":\"console_cmd\"") != std::string::npos) {
        if (cmd.find("\"input\":\"ping\"") != std::string::npos) response = "{\"status\":\"ok\",\"msg\":\"pong\"}";
        else if (cmd.find("\"input\":\"world\"") != std::string::npos) {
            uintptr_t b = g_player_base.load(); float x = 0, y = 0, z = 0;
            if (b) { float p[3]; if (safe_read_floats(b, p, 3)) { x = p[0];y = p[1];z = p[2]; } }
            char buf[128]; snprintf(buf, sizeof(buf), "{\"status\":\"ok\",\"msg\":\"0x%llx (%.1f, %.1f, %.1f)\"}", b, x, y, z); response = buf;
        }
        else if (cmd.find("\"input\":\"help\"") != std::string::npos) response = "{\"status\":\"ok\",\"msg\":\"Commands: ping, world, help\"}";
        else if (cmd.find("orbit_target") != std::string::npos) {
            uint64_t targetId = 0; sscanf_s(cmd.c_str(), "{\"cmd\":\"console_cmd\",\"input\":\"orbit_target %llu\"}", &targetId);
            if (targetId > 0) {
                std::lock_guard<std::mutex> lk(g_net_mutex);
                for (int i = 0; i < g_net_entity_count.load(); i++) { if (g_net_entities[i].id == targetId) { g_orbit_target.store(i); response = "{\"status\":\"ok\",\"msg\":\"orbit target set\"}"; break; } }
            }
            else response = "{\"status\":\"error\",\"msg\":\"invalid id\"}";
        }
        else response = "{\"status\":\"ok\",\"msg\":\"unknown\"}";
    }
    else response = "{\"status\":\"error\",\"msg\":\"unknown\"}";
    return response + "\n";
}

static DWORD WINAPI IpcConnectionThread(LPVOID param) {
    HANDLE pipe = (HANDLE)param;
    auto buffer = std::make_unique<char[]>(65536);
    while (g_ipc_running.load() && !g_shutdown.load()) {
        DWORD bytes_read = 0;
        if (!ReadFile(pipe, buffer.get(), 65535, &bytes_read, nullptr)) break;
        buffer[bytes_read] = '\0';
        std::string cmd(buffer.get(), bytes_read);
        std::string response = handle_command(cmd);
        DWORD written = 0;
        WriteFile(pipe, response.c_str(), static_cast<DWORD>(response.size()), &written, nullptr);
    }
    FlushFileBuffers(pipe);
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
    return 0;
}

void ipc_server_thread() {
    char pipe_name[64];
    sprintf_s(pipe_name, sizeof(pipe_name), "\\\\.\\pipe\\durag_executor_%lu", GetCurrentProcessId());
    while (g_ipc_running.load() && !g_shutdown.load()) {
        HANDLE pipe = CreateNamedPipeA(pipe_name, PIPE_ACCESS_DUPLEX, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, PIPE_UNLIMITED_INSTANCES, 65536, 65536, 1000, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) { Sleep(200); continue; }
        BOOL connected = ConnectNamedPipe(pipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (connected) {
            HANDLE h = CreateThread(nullptr, 0, IpcConnectionThread, (LPVOID)pipe, 0, nullptr);
            if (h) CloseHandle(h);
        }
        else {
            CloseHandle(pipe);
        }
    }
}

// =============================================================================
// DLL ENTRY
// =============================================================================

void Initialize() {
    if (g_initialized.load()) return;
    g_initialized.store(true);
    OutputDebugStringA("[Durag] Initializing...");
    if (hook_dxgi_present()) OutputDebugStringA("[Durag] DXGI Present hook installed");
    InstallWriteHook();
    Script_Init();
    CreateThread(nullptr, 0, HwbpWatchdogThread, nullptr, 0, nullptr);
    timeBeginPeriod(1);
    if (hook_winsock()) OutputDebugStringA("[Durag] Winsock hook installed");
    CreateThread(nullptr, 0, MovementSpamThread, nullptr, 0, nullptr);
    g_ipc_running.store(true);
    CreateThread(nullptr, 0, [](LPVOID) -> DWORD { ipc_server_thread(); return 0; }, nullptr, 0, nullptr);
    OutputDebugStringA("[Durag] IPC server started");
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, LPVOID lpReserved) {
    switch (dwReason) {
    case DLL_PROCESS_ATTACH:
        g_hModule = hModule;
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, [](LPVOID) -> DWORD { Sleep(2000); Initialize(); return 0; }, nullptr, 0, nullptr);
        break;
    case DLL_PROCESS_DETACH:
        g_shutdown.store(true);
        g_ipc_running.store(false);
        g_movement_thread_running.store(false);
        if (g_initialized.load()) { MH_DisableHook(MH_ALL_HOOKS); MH_Uninitialize(); timeEndPeriod(1); }
        break;
    }
    return TRUE;
}