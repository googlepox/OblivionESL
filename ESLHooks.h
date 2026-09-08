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

    // int __thiscall SaveLoad_ResolveFormID(TESSaveLoad* this, int formID)
    typedef int(__fastcall* tSaveLoadResolveFormID)(
        void* saveLoad,
        void* edx,
        int formID
        );

    // int __thiscall TESDataHandler::LoadFile(ModEntry::Data* file, char flag)
    typedef int(__fastcall* tLoadFile)(
        void* thisPtr,
        void* edx,
        void* tesFile,
        char flag
        );

    // ── Original function pointers (populated by InstallHooks) ───────────────

    extern tResolveFormID g_ResolveFormID;
    extern tSaveFormID    g_SaveFormID;
    extern tSetFormID     g_SetFormID;
    extern tLoadFile      g_LoadFile;
    extern tSaveLoadResolveFormID g_SaveLoadResolveFormID;

    // ── Hook implementations ──────────────────────────────────────────────────

    void __cdecl   ResolveFormID_Hook(UInt32* formID, ModEntry::Data* file);
    void __fastcall SaveFormID_Hook(void* thisPtr, void* edx, void* src, UInt32 size);
    void __fastcall SetFormID_Hook(TESForm* form, void* edx, UInt32 newID, bool releaseOld);
    int  __fastcall LoadFile_Hook(void* thisPtr, void* edx, void* tesFile, char flag);
    int  __fastcall SaveLoadResolveFormID_Hook(void* saveLoad, void* edx, int formID);

    // ── Setup ──────────────────────────────────────────────────────────────────

    bool InstallHooks();

}