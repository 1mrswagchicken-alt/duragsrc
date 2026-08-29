#pragma once
#include <windows.h>
#include <psapi.h>
#include <vector>
#include <cstdint>
#include <cstring>
#include <cstdlib>

struct Signature {
    std::vector<uint8_t> bytes;
    std::vector<bool> mask;

    static Signature Parse(const char* pattern) {
        Signature sig;
        const char* p = pattern;
        while (*p) {
            while (*p == ' ') p++;
            if (!*p) break;
            if (*p == '?') {
                sig.bytes.push_back(0x00);
                sig.mask.push_back(false);
                if (p[1] == '?') p++;
                p++;
            }
            else {
                char hex[3] = { p[0], p[1], 0 };
                sig.bytes.push_back((uint8_t)strtoul(hex, nullptr, 16));
                sig.mask.push_back(true);
                p += 2;
            }
        }
        return sig;
    }

    uint8_t* Scan(uint8_t* start, size_t size) const {
        size_t plen = bytes.size();
        if (plen == 0 || size < plen) return nullptr;
        for (size_t i = 0; i <= size - plen; i++) {
            bool found = true;
            for (size_t j = 0; j < plen; j++) {
                if (mask[j] && start[i + j] != bytes[j]) {
                    found = false;
                    break;
                }
            }
            if (found) return start + i;
        }
        return nullptr;
    }
};

static uint8_t* ScanModule(HMODULE module, const char* pattern) {
    if (!module) return nullptr;
    MODULEINFO modInfo;
    if (!GetModuleInformation(GetCurrentProcess(), module, &modInfo, sizeof(modInfo))) return nullptr;
    Signature sig = Signature::Parse(pattern);
    return sig.Scan((uint8_t*)module, modInfo.SizeOfImage);
}

static uint8_t* ScanAllMemory(const char* pattern) {
    Signature sig = Signature::Parse(pattern);
    if (sig.bytes.empty()) return nullptr;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uint8_t* addr = (uint8_t*)si.lpMinimumApplicationAddress;
    uint8_t* maxAddr = (uint8_t*)si.lpMaximumApplicationAddress;
    MEMORY_BASIC_INFORMATION mbi;
    while (addr < maxAddr) {
        if (VirtualQuery(addr, &mbi, sizeof(mbi)) == 0) break;
        if (mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY | PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY))) {
            uint8_t* result = sig.Scan((uint8_t*)mbi.BaseAddress, mbi.RegionSize);
            if (result) return result;
        }
        addr = (uint8_t*)mbi.BaseAddress + mbi.RegionSize;
    }
    return nullptr;
}