#pragma once
#include "obse/GameForms.h"
#include "obse/GameObjects.h"
#include "obse/GameData.h"
#include <cstdint>

namespace ESLHooks {

    // ── Original-function typedefs (signatures match the vanilla functions
    // being detoured, per the disassembly reviewed earlier) ──────────────────

    // unsigned int __cdecl TESForm_ResolveFormID(UInt32* formID, ModEntry::Data* file)
    typedef void(__cdecl* tResolveFormID)(
        UInt32* formID,
        ModEntry::Data* file
        );

    // void __thiscall SaveFormID(void* this, void* src, UInt32 size)
    //
    // The edx placeholder is REQUIRED, not cosmetic. __fastcall passes the
    // first TWO integer args in ecx and edx; __thiscall passes only `this` in
    // ecx with everything else on the stack. Without a dummy to absorb edx,
    // the second real argument lands in a register instead of on the stack and
    // every stack argument shifts by one slot.
    typedef void(__fastcall* tSaveFormID)(
        void* thisPtr,
        void* edx,
        void* src,
        UInt32 size
        );

    // void __thiscall TESForm::SetFormID(UInt32 newID, bool releaseOldID)
    typedef void(__fastcall* tSetFormID)(
        TESForm* form,
        void* edx,
        UInt32 newID,
        bool releaseOld
        );

    // bool __stdcall TESDataHandler_IsFormIDCreated(UInt32 formID)
    // Note __stdcall, not __cdecl -- it ends in "retn 4", so the callee cleans.
    typedef bool(__stdcall* tIsFormIDCreated)(UInt32 formID);

    // UInt32 __thiscall SaveLoad_IRefToFormID(TESSaveLoad* this, UInt32 formID)
    // (0x0045E0D0) -- the SAVE side of the iref table: FormID -> index,
    // appending a new entry when the FormID is not already present.
    typedef UInt32(__fastcall* tIRefToFormID)(
        void* saveLoad,
        void* edx,
        UInt32 formID
        );

    // UInt32 __thiscall SaveLoad_RemapSavedFormID(TESSaveLoad* this, UInt32 formID)
    // (sub_459950) -- a second, separate remapping path used while loading a
    // save, for cell and worldspace references among others.
    typedef UInt32(__fastcall* tRemapSavedFormID)(
        void* saveLoad,
        void* edx,
        UInt32 formID
        );

    // int __thiscall SaveLoad_ResolveFormID(TESSaveLoad* this, int formID)
    typedef int(__fastcall* tSaveLoadResolveFormID)(
        void* saveLoad,
        void* edx,
        int formID
        );

    // ── Original function pointers (populated by InstallHooks) ───────────────

    extern tResolveFormID g_ResolveFormID;
    extern tSaveFormID    g_SaveFormID;
    extern tSetFormID     g_SetFormID;
    extern tSaveLoadResolveFormID g_SaveLoadResolveFormID;
    extern tRemapSavedFormID g_RemapSavedFormID;
    extern tIsFormIDCreated g_IsFormIDCreated;
    extern tIRefToFormID g_IRefToFormID;

    // ── Hook implementations ──────────────────────────────────────────────────

    void __cdecl   ResolveFormID_Hook(UInt32* formID, ModEntry::Data* file);
    void __fastcall SaveFormID_Hook(void* thisPtr, void* edx, void* src, UInt32 size);
    void __fastcall SetFormID_Hook(TESForm* form, void* edx, UInt32 newID, bool releaseOld);
    int  __fastcall SaveLoadResolveFormID_Hook(void* saveLoad, void* edx, int formID);
    UInt32 __fastcall RemapSavedFormID_Hook(void* saveLoad, void* edx, UInt32 formID);
    bool __stdcall IsFormIDCreated_Hook(UInt32 formID);
    UInt32 __fastcall IRefToFormID_Hook(void* saveLoad, void* edx, UInt32 formID);

    // ── Helpers ────────────────────────────────────────────────────────────────

    // True if this plugin carries the ESL header flag. Reads the flag straight
    // from the file (the engine does not preserve it in ModEntry::Data::flags),
    // with results cached per file. Used by ESLLoadPatch to decide whether a
    // plugin gets a load order slot.
    bool IsESLFile(ModEntry::Data* file);

    // Drops the IsESLFile result cache.
    //
    // Must be called on teardown alongside ESLManager::ClearRuntimeState() and
    // ESLLoadPatch::ClearFileList(). The cache is keyed by ModEntry::Data*, and
    // the engine frees those objects when returning to the main menu -- so a
    // later allocation landing on the same address would inherit a stale
    // answer about whether it is an ESL.
    void ClearESLCache();

    // ── Setup ──────────────────────────────────────────────────────────────────

    bool InstallHooks();

}