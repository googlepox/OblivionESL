#pragma once
#include "obse/GameData.h"
#include <vector>

namespace ESLLoadPatch {

    bool InstallPatches();

    extern ModEntry::Data* g_currentLoadingESL;

    UInt32 GetESLCount();

    std::vector<ModEntry::Data*> GetESLFiles();

    void ClearFileList();

    void ResetLoadedFlags();

    typedef int(__fastcall* tReloadAllFiles)(void* thisPtr, void* edx);

    extern tReloadAllFiles g_ReloadAllFiles;

    int __fastcall ReloadAllFiles_Hook(void* thisPtr, void* edx);

    typedef UInt32(__fastcall* tGetModCount)(void* dataHandler, void* edx);
    typedef void* (__fastcall* tGetNthMod)(void* dataHandler, void* edx, UInt32 index);

    extern tGetModCount g_GetModCount;
    extern tGetNthMod   g_GetNthMod;

    UInt32 __fastcall GetModCount_Hook(void* dataHandler, void* edx);
    void* __fastcall GetNthMod_Hook(void* dataHandler, void* edx, UInt32 index);

    bool InstallReloadHook();

    void UnlinkESLsFromModList();
    void RelinkESLsToModList();
}