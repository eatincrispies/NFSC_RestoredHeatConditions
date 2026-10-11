#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "NFSC/SoundAI.hpp"

namespace {

    HMODULE gModule = nullptr;

    DWORD WINAPI Initialize(void*) {
        Sleep(1000);
        SoundAI::InstallPatches(gModule);
        return 0;
    }

}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        gModule = module;
        DisableThreadLibraryCalls(module);
        if (HANDLE thread = CreateThread(nullptr, 0, Initialize, nullptr, 0, nullptr)) CloseHandle(thread);
        break;

    case DLL_PROCESS_DETACH:
        if (reserved == nullptr) SoundAI::RestorePatches();
        break;
    }
    return TRUE;
}
