#pragma once
#include "obse/GameForms.h"
#include "obse/GameObjects.h"
#include "obse/GameData.h"
#include <cstdint>

namespace ESLHooks {

    namespace ESLDetour
    {
        void* WriteDetour(void* target, void* hook, size_t patchSize);
    }

    typedef void(__cdecl* tResolveFormID)(
        UInt32* formID,
        ModEntry::Data* file
        );

    typedef void(__fastcall* tSaveFormID)(
        void* thisPtr,
        void* edx,
        void* src,
        UInt32 size
        );

    typedef void(__fastcall* tSetFormID)(
        TESForm* form,
        void* edx,
        UInt32 newID,
        bool releaseOld
        );


    typedef UInt32(__fastcall* tIRefToFormID)(
        void* saveLoad,
        void* edx,
        UInt32 formID
        );

    typedef UInt32(__fastcall* tRemapSavedFormID)(
        void* saveLoad,
        void* edx,
        UInt32 formID
        );

    typedef int(__fastcall* tSaveLoadResolveFormID)(
        void* saveLoad,
        void* edx,
        int formID
        );

    extern tResolveFormID g_ResolveFormID;
    extern tSaveFormID    g_SaveFormID;
    extern tSetFormID     g_SetFormID;
    extern tSaveLoadResolveFormID g_SaveLoadResolveFormID;
    extern tRemapSavedFormID g_RemapSavedFormID;
    extern tIRefToFormID g_IRefToFormID;

    int __fastcall LoadFormRecord_Hook(
        void* dataHandler, void* edx, ModEntry::Data* file, char flag);

    void __cdecl   ResolveFormID_Hook(UInt32* formID, ModEntry::Data* file);
    void __fastcall SaveFormID_Hook(void* thisPtr, void* edx, void* src, UInt32 size);
    void __fastcall SetFormID_Hook(TESForm* form, void* edx, UInt32 newID, bool releaseOld);
    int  __fastcall SaveLoadResolveFormID_Hook(void* saveLoad, void* edx, int formID);
    UInt32 __fastcall RemapSavedFormID_Hook(void* saveLoad, void* edx, UInt32 formID);
    UInt32 __fastcall IRefToFormID_Hook(void* saveLoad, void* edx, UInt32 formID);

    bool IsESLFile(ModEntry::Data* file);
    void ClearESLCache();
    bool InstallHooks();

}