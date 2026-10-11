#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "SoundAI.hpp"

namespace Log {

#if defined(_DEBUG)
    namespace {

        FILE* gFile = nullptr;

    }

    void Open(void* module) {
        char path[MAX_PATH] = "";
        const DWORD length = GetModuleFileNameA(static_cast<HMODULE>(module), path, MAX_PATH);
        char* dot = length && length < MAX_PATH ? std::strrchr(path, '.') : nullptr;
        if (!dot || dot < std::strrchr(path, '\\')) return;
        std::strcpy(dot, ".log");
        gFile = std::fopen(path, "w");
    }

    void Close() {
        if (gFile) std::fclose(gFile);
        gFile = nullptr;
    }

    void Write(const char* format, ...) {
        if (!gFile) return;
        char line[256] = "";
        va_list args;
        va_start(args, format);
        std::vsnprintf(line, sizeof(line) - 2, format, args);
        va_end(args);
        std::strcat(line, "\n");
        std::fputs(line, gFile);
        std::fflush(gFile);
    }
#else
    void Open(void*) {}
    void Close() {}
    void Write(const char* format, ...);
#endif

}

#if defined(_DEBUG)
#define LOG(...) Log::Write(__VA_ARGS__)
#else
#define LOG(...) ((void)sizeof((Log::Write(__VA_ARGS__), 0)))
#endif

namespace Memory {

    namespace {

        int SafeCopy(void* destination, const void* source, size_t length) {
            __try {
                std::memcpy(destination, source, length);
                return 1;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                return 0;
            }
        }

    }

    bool Read(uintptr_t va, void* out, size_t length) {
        return SafeCopy(out, reinterpret_cast<const void*>(va), length) != 0;
    }

    bool Matches(uintptr_t va, const void* expected, size_t length) {
        uint8_t bytes[64] = {};
        return length <= sizeof(bytes) && Read(va, bytes, length) && std::memcmp(bytes, expected, length) == 0;
    }

    bool WriteCode(uintptr_t va, const void* bytes, size_t length) {
        void* target = reinterpret_cast<void*>(va);
        DWORD oldProtect = 0;
        if (!VirtualProtect(target, length, PAGE_EXECUTE_READWRITE, &oldProtect)) return false;
        std::memcpy(target, bytes, length);
        FlushInstructionCache(GetCurrentProcess(), target, length);
        DWORD unused = 0;
        VirtualProtect(target, length, oldProtect, &unused);
        return true;
    }

}

namespace Patch {

    struct CodeSite {
        uintptr_t va;
        uint8_t   code[7];
        size_t    length;
        size_t    operand;
    };

    namespace {

        constexpr int kMostPatches = 8;

        struct Original {
            uintptr_t va;
            uint32_t  value;
        };

        Original gOriginals[kMostPatches] = {};
        int      gCount = 0;

        uint32_t CallOffset(uintptr_t call, uintptr_t target) {
            return static_cast<uint32_t>(target) - static_cast<uint32_t>(call + 5);
        }

    }

    bool IsGameCode(const CodeSite& site) {
        return Memory::Matches(site.va, site.code, site.length);
    }

    bool CallsTo(const CodeSite& call, uintptr_t function) {
        uint32_t offset = 0;
        return IsGameCode(call) && Memory::Read(call.va + call.operand, &offset, sizeof(offset)) && offset == CallOffset(call.va, function);
    }

    bool WriteOperand(const CodeSite& site, uint32_t value) {
        const uintptr_t va = site.va + site.operand;
        uint32_t original = 0;
        if (gCount >= kMostPatches || !Memory::Read(va, &original, sizeof(original)) || !Memory::WriteCode(va, &value, sizeof(value)))
            return false;
        gOriginals[gCount++] = { va, original };
        return true;
    }

    bool RedirectCall(const CodeSite& call, const void* replacement) {
        return WriteOperand(call, CallOffset(call.va, reinterpret_cast<uintptr_t>(replacement)));
    }

    void RestoreAll() {
        while (gCount > 0) {
            const Original& original = gOriginals[--gCount];
            Memory::WriteCode(original.va, &original.value, sizeof(original.value));
        }
    }

}

namespace Game {

    template <typename Return, typename... Args>
    Return ThisCall(uintptr_t function, Args... args) {
        return reinterpret_cast<Return (__thiscall*)(Args...)>(function)(args...);
    }

    template <typename Method>
    const void* MethodAddress(Method method) {
        static_assert(sizeof(method) == sizeof(void*), "only plain member functions have a single code address");
        const void* address = nullptr;
        std::memcpy(&address, &method, sizeof(address));
        return address;
    }

    template <typename T>
    T& Global(uintptr_t va) {
        return *reinterpret_cast<T*>(va);
    }

}

namespace {

    constexpr const char* kVersion = "V1.0.0";

    constexpr uintptr_t kSyncPlayers         = 0x00793840u;
    constexpr uintptr_t kHeatCutoffs         = 0x009ECCB4u;
    constexpr uintptr_t kSyncPlayersPrevHeat = 0x00A71BA8u;

    constexpr Patch::CodeSite kSyncPlayersCutoffLoop = { 0x00793C59u, { 0xBD, 0x03, 0x00, 0x00, 0x00 }, 5, 1 };

    constexpr Patch::CodeSite kSyncPlayersCutoffValues[] = {
        { 0x00793C66u, { 0xD8, 0x1C, 0xED, 0xB4, 0xCC, 0x9E, 0x00 }, 7, 3 },
        { 0x00793C7Bu, { 0xD8, 0x1C, 0xED, 0xB4, 0xCC, 0x9E, 0x00 }, 7, 3 },
    };

    constexpr Patch::CodeSite kSyncPlayersCutoffHeatLevels[] = {
        { 0x00793CCBu, { 0x8B, 0x0C, 0xED, 0xB8, 0xCC, 0x9E, 0x00 }, 7, 3 },
        { 0x00793D23u, { 0x8B, 0x04, 0xED, 0xB8, 0xCC, 0x9E, 0x00 }, 7, 3 },
    };

    constexpr Patch::CodeSite kOnTaskSyncPlayers = { 0x007984F8u, { 0xE8 }, 1, 1 };

    constexpr SoundAI::HeatCutoffs kGameHeatCutoffs[] = {
        { 2.0f, Csis::Type_heat_level_2 },
        { 3.0f, Csis::Type_heat_level_3 },
        { 4.0f, Csis::Type_heat_level_4 },
        { 5.0f, Csis::Type_heat_level_5 },
    };

    constexpr SoundAI::HeatCutoffs kRestoredHeatCutoffs[] = {
        { 2.0f, Csis::Type_heat_level_2 },  { 3.0f, Csis::Type_heat_level_3 },  { 4.0f, Csis::Type_heat_level_4 },
        { 5.0f, Csis::Type_heat_level_5 },  { 6.0f, Csis::Type_heat_level_6 },  { 7.0f, Csis::Type_heat_level_7 },
        { 8.0f, Csis::Type_heat_level_8 },  { 9.0f, Csis::Type_heat_level_9 },  { 10.0f, Csis::Type_heat_level_10 },
    };

    constexpr uint32_t kLastRestoredCutoff = sizeof(kRestoredHeatCutoffs) / sizeof(kRestoredHeatCutoffs[0]) - 1;

    static_assert(sizeof(SoundAI::HeatCutoffs) == 8, "SoundAI::heat_cutoffs entries are 8 bytes");
    static_assert(offsetof(SoundAI, mPlayerHeat) == 0x100, "SoundAI::mPlayerHeat");

    uint32_t Address(const void* pointer) {
        return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pointer));
    }

    float& prev_heat() {
        return Game::Global<float>(kSyncPlayersPrevHeat);
    }

    bool GameCodeIsVanilla() {
        if (!Memory::Matches(kHeatCutoffs, kGameHeatCutoffs, sizeof(kGameHeatCutoffs))) {
            LOG("Heat jump: SoundAI::heat_cutoffs is not the game's own table, so nothing is changed.");
            return false;
        }
        bool vanilla = Patch::IsGameCode(kSyncPlayersCutoffLoop) && Patch::CallsTo(kOnTaskSyncPlayers, kSyncPlayers);
        for (const Patch::CodeSite& site : kSyncPlayersCutoffValues) vanilla = vanilla && Patch::IsGameCode(site);
        for (const Patch::CodeSite& site : kSyncPlayersCutoffHeatLevels) vanilla = vanilla && Patch::IsGameCode(site);
        if (!vanilla) LOG("Heat jump: another mod already changed SoundAI::SyncPlayers, so nothing is changed.");
        return vanilla;
    }

    bool UseRestoredHeatCutoffs() {
        bool applied = Patch::WriteOperand(kSyncPlayersCutoffLoop, kLastRestoredCutoff);
        for (const Patch::CodeSite& site : kSyncPlayersCutoffValues)
            applied = applied && Patch::WriteOperand(site, Address(&kRestoredHeatCutoffs[0].value));
        for (const Patch::CodeSite& site : kSyncPlayersCutoffHeatLevels)
            applied = applied && Patch::WriteOperand(site, Address(&kRestoredHeatCutoffs[0].heat_level));
        return applied;
    }

}

bool SoundAI::InstallPatches(void* module) {
    Log::Open(module);
    LOG("NFSC_RestoreHeatConditions %s starting.", kVersion);
    if (!GameCodeIsVanilla()) return false;
    if (!UseRestoredHeatCutoffs() || !Patch::RedirectCall(kOnTaskSyncPlayers, Game::MethodAddress(&SoundAI::SyncPlayersAndRearmHeatJump))) {
        LOG("Heat jump: the game's code could not be changed.");
        RestorePatches();
        return false;
    }
    LOG("Heat jump: restored the radio calls for reaching heat 6, 7, 8, 9 and 10.");
    return true;
}

void SoundAI::RestorePatches() {
    Patch::RestoreAll();
    Log::Close();
}

void SoundAI::SyncPlayers() {
    Game::ThisCall<void>(kSyncPlayers, this);
}

void SoundAI::SyncPlayersAndRearmHeatJump() {
    const float before = prev_heat();
    SyncPlayers();
    const float after = prev_heat();
    if (after != before) LOG("Heat jump: the radio calls out your heat jump to %.1f.", after);

    if (mPlayerHeat < 1 || mPlayerHeat >= static_cast<int>(after)) return;
    prev_heat() = static_cast<float>(mPlayerHeat);
    LOG("Heat jump: your heat dropped to %d, so the next jump above it gets its call.", mPlayerHeat);
}
