#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "SoundAI.hpp"

using namespace Csis;

static constexpr uintptr_t kAddrSyncPlayers = 0x00793840u;
static constexpr uintptr_t kAddrHeatCutoffs = 0x009ECCB4u;
static constexpr uintptr_t kAddrPrevHeat    = 0x00A71BA8u;

static constexpr SoundAI::HeatCutoffs kRestoredCutoffs[] = {
    {  2.0f, Type_heat_level_2  },
    {  3.0f, Type_heat_level_3  },
    {  4.0f, Type_heat_level_4  },
    {  5.0f, Type_heat_level_5  },
    {  6.0f, Type_heat_level_6  },
    {  7.0f, Type_heat_level_7  },
    {  8.0f, Type_heat_level_8  },
    {  9.0f, Type_heat_level_9  },
    { 10.0f, Type_heat_level_10 },
};

static_assert(sizeof(SoundAI::HeatCutoffs) == 8, "SoundAI::heat_cutoffs entries are 8 bytes");
static_assert(offsetof(SoundAI, mPlayerHeat) == 0x100, "SoundAI::mPlayerHeat");

#ifdef _DEBUG
static constexpr const char* kVersion = "V1.0.0";
static FILE* gLogFile = nullptr;

static void DebugLog(const char* fmt, ...) {
    if (!gLogFile) return;
    char line[256] = "";
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(line, sizeof(line) - 2, fmt, args);
    va_end(args);
    std::strcat(line, "\n");
    std::fputs(line, gLogFile);
    std::fflush(gLogFile);
}

#define LOG(...) DebugLog(__VA_ARGS__)
#else
#define LOG(...) ((void)0)
#endif

struct SavedDword {
    uintptr_t address;
    uint32_t  value;
};

static SavedDword gSaved[8]   = {};
static int        gSavedCount = 0;

static int SafeCopy(void* dst, const void* src, size_t size) {
    __try {
        std::memcpy(dst, src, size);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static bool SafeRead(uintptr_t addr, void* out, size_t size) {
    return SafeCopy(out, reinterpret_cast<const void*>(addr), size) != 0;
}

static bool BytesMatch(uintptr_t addr, const void* expected, size_t size) {
    uint8_t buf[64] = {};
    return size <= sizeof(buf) && SafeRead(addr, buf, size) && std::memcmp(buf, expected, size) == 0;
}

static bool WriteMemory(uintptr_t addr, const void* data, size_t size) {
    void* target  = reinterpret_cast<void*>(addr);
    DWORD oldProt = 0;
    if (!VirtualProtect(target, size, PAGE_EXECUTE_READWRITE, &oldProt)) return false;
    std::memcpy(target, data, size);
    FlushInstructionCache(GetCurrentProcess(), target, size);
    DWORD unused = 0;
    VirtualProtect(target, size, oldProt, &unused);
    return true;
}

static uint32_t Rel32(uintptr_t callSite, uintptr_t target) {
    return static_cast<uint32_t>(target) - static_cast<uint32_t>(callSite + 5);
}

static bool IsCallTo(uintptr_t callSite, uintptr_t target) {
    uint8_t insn[5] = {};
    if (!SafeRead(callSite, insn, sizeof(insn)) || insn[0] != 0xE8) return false;
    uint32_t rel = 0;
    std::memcpy(&rel, insn + 1, sizeof(rel));
    return rel == Rel32(callSite, target);
}

static bool PatchDword(uintptr_t addr, uint32_t val) {
    if (gSavedCount >= static_cast<int>(sizeof(gSaved) / sizeof(gSaved[0]))) return false;
    uint32_t orig = 0;
    if (!SafeRead(addr, &orig, sizeof(orig)) || !WriteMemory(addr, &val, sizeof(val))) return false;
    gSaved[gSavedCount++] = { addr, orig };
    return true;
}

static bool IsVanillaCode() {
    if (!BytesMatch(kAddrHeatCutoffs, kRestoredCutoffs, 4 * sizeof(kRestoredCutoffs[0]))) {
        LOG("Heat jump: SoundAI::heat_cutoffs is not the game's own table, so nothing is changed.");
        return false;
    }

    const uint8_t movEbp3[]    = { 0xBD, 0x03, 0x00, 0x00, 0x00 };
    const uint8_t fcompValue[] = { 0xD8, 0x1C, 0xED, 0xB4, 0xCC, 0x9E, 0x00 };
    const uint8_t movEcxLvl[]  = { 0x8B, 0x0C, 0xED, 0xB8, 0xCC, 0x9E, 0x00 };
    const uint8_t movEaxLvl[]  = { 0x8B, 0x04, 0xED, 0xB8, 0xCC, 0x9E, 0x00 };

    const bool ok =
        BytesMatch(0x00793C59u, movEbp3, sizeof(movEbp3)) &&
        BytesMatch(0x00793C66u, fcompValue, sizeof(fcompValue)) &&
        BytesMatch(0x00793C7Bu, fcompValue, sizeof(fcompValue)) &&
        BytesMatch(0x00793CCBu, movEcxLvl, sizeof(movEcxLvl)) &&
        BytesMatch(0x00793D23u, movEaxLvl, sizeof(movEaxLvl)) &&
        IsCallTo(0x007984F8u, kAddrSyncPlayers);

    if (!ok) LOG("Heat jump: another mod already changed SoundAI::SyncPlayers, so nothing is changed.");
    return ok;
}

static bool ApplyPatches() {
    constexpr uint32_t lastIdx = static_cast<uint32_t>(sizeof(kRestoredCutoffs) / sizeof(kRestoredCutoffs[0]) - 1);
    const uint32_t valuesAddr  = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&kRestoredCutoffs[0].value));
    const uint32_t levelsAddr  = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&kRestoredCutoffs[0].heat_level));

    auto hook = &SoundAI::SyncPlayersAndRearmHeatJump;
    uintptr_t hookAddr = 0;
    static_assert(sizeof(hook) == sizeof(hookAddr), "only plain member functions have a single address");
    std::memcpy(&hookAddr, &hook, sizeof(hookAddr));

    return PatchDword(0x00793C59u + 1, lastIdx) &&
           PatchDword(0x00793C66u + 3, valuesAddr) &&
           PatchDword(0x00793C7Bu + 3, valuesAddr) &&
           PatchDword(0x00793CCBu + 3, levelsAddr) &&
           PatchDword(0x00793D23u + 3, levelsAddr) &&
           PatchDword(0x007984F8u + 1, Rel32(0x007984F8u, hookAddr));
}

bool SoundAI::InstallPatches(void* module) {
#ifdef _DEBUG
    char path[MAX_PATH] = "";
    DWORD len = GetModuleFileNameA(static_cast<HMODULE>(module), path, MAX_PATH);
    char* dot = (len > 0 && len < MAX_PATH) ? std::strrchr(path, '.') : nullptr;
    if (dot && dot >= std::strrchr(path, '\\') && static_cast<size_t>(dot - path) + sizeof(".log") <= sizeof(path)) {
        std::strcpy(dot, ".log");
        gLogFile = std::fopen(path, "w");
    }
    LOG("NFSC_RestoreHeatConditions %s starting.", kVersion);
#else
    (void)module;
#endif

    if (!IsVanillaCode()) return false;
    if (!ApplyPatches()) {
        LOG("Heat jump: the game's code could not be changed.");
        RestorePatches();
        return false;
    }
    LOG("Heat jump: restored the radio calls for reaching heat 6, 7, 8, 9 and 10.");
    return true;
}

void SoundAI::RestorePatches() {
    while (gSavedCount > 0) {
        const SavedDword& patch = gSaved[--gSavedCount];
        WriteMemory(patch.address, &patch.value, sizeof(patch.value));
    }
#ifdef _DEBUG
    if (gLogFile) {
        std::fclose(gLogFile);
        gLogFile = nullptr;
    }
#endif
}

void SoundAI::SyncPlayers() {
    reinterpret_cast<void(__thiscall*)(SoundAI*)>(kAddrSyncPlayers)(this);
}

void SoundAI::SyncPlayersAndRearmHeatJump() {
    float& prevHeat = *reinterpret_cast<float*>(kAddrPrevHeat);
    const float before = prevHeat;
    SyncPlayers();
    const float after = prevHeat;

    if (after != before) LOG("Heat jump: the radio calls out your heat jump to %.1f.", after);
    if (mPlayerHeat < 1 || mPlayerHeat >= static_cast<int>(after)) return;

    prevHeat = static_cast<float>(mPlayerHeat);
    LOG("Heat jump: your heat dropped to %d, so the next jump above it gets its call.", mPlayerHeat);
}