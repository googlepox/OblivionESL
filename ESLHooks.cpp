#include "ESLHooks.h"
#include "ESLManager.h"
#include "ESLLoadPatch.h"
#include "obse/GameForms.h"
#include "obse/GameObjects.h"
#include "obse/GameData.h"
#include "obse_common/SafeWrite.h"
#include "obse/PluginAPI.h"
#include <windows.h>
#include <cstring>
#include <cstdio>
#include <unordered_map>

namespace ESLHooks {

    tResolveFormID g_ResolveFormID = nullptr;
    tSaveFormID    g_SaveFormID = nullptr;
    tSetFormID     g_SetFormID = nullptr;
    tSaveLoadResolveFormID g_SaveLoadResolveFormID = nullptr;
    tRemapSavedFormID g_RemapSavedFormID = nullptr;
    tIsFormIDCreated g_IsFormIDCreated = nullptr;
    tIRefToFormID g_IRefToFormID = nullptr;

    constexpr UInt32 kESLFlag = 0x00080000;

    // TESFile_GetMasterByIndex(file, oneBasedIndex) -> ModEntry::Data* or null.
    // Same function TESForm_ResolveFormID calls. Needed to identify WHICH file
    // a reference targets, since vanilla only tells us the resulting index byte.
    // TODO: address from IDA.
    typedef ModEntry::Data* (__fastcall* tGetMasterByIndex)(
        ModEntry::Data* file, void* edx, UInt32 index);

    tGetMasterByIndex GetMasterByIndex = (tGetMasterByIndex)0x0044FD60;

    namespace ESLDetour
    {
        inline void* WriteDetour(void* target, void* hook, size_t patchSize)
        {
            if (patchSize < 5)
                return nullptr;

            DWORD oldProtect;
            VirtualProtect(target, patchSize, PAGE_EXECUTE_READWRITE, &oldProtect);

            // Allocate trampoline
            BYTE* trampoline = (BYTE*)VirtualAlloc(
                nullptr,
                patchSize + 5,
                MEM_COMMIT | MEM_RESERVE,
                PAGE_EXECUTE_READWRITE
            );

            if (!trampoline)
                return nullptr;

            // Copy original bytes
            // NOTE: verify none of the copied bytes are relative CALL/JMP/LEA
            // instructions before shipping — those would be corrupted by
            // relocation to the trampoline's address.
            memcpy(trampoline, target, patchSize);

            uintptr_t returnAddr = (uintptr_t)target + patchSize;

            trampoline[patchSize] = 0xE9;
            *(uint32_t*)(trampoline + patchSize + 1) =
                returnAddr - ((uintptr_t)trampoline + patchSize + 5);

            *(BYTE*)target = 0xE9;
            *(uint32_t*)((uintptr_t)target + 1) =
                (uintptr_t)hook - ((uintptr_t)target + 5);

            for (size_t i = 5; i < patchSize; i++)
                *((BYTE*)target + i) = 0x90;

            VirtualProtect(target, patchSize, oldProtect, &oldProtect);

            return trampoline;
        }
    }

    // ── Shared helpers ──────────────────────────────────────────────────────────

    // Detection is by header flag, NOT by file extension.
    //
    // Oblivion's engine enumerates Data for .esp and .esm only -- it has no
    // concept of a .esl extension, so such a file would never be picked up
    // without additional hooks into the plugin scanning code. A flagged .esp
    // loads normally and lets these hooks decide what it means.
    //
    // ModEntry::Data::flags (offset 0x3DC) CANNOT be used for this. Despite
    // kFlag_IsMaster being bit 0, the engine does not copy the TES4 flags
    // dword verbatim -- it extracts the bits it cares about and ORs in its own
    // runtime state. Observed across a 43-plugin load order: every .esp reads
    // 0x00000006 and every .esm reads 0x00000007, regardless of what the file
    // actually holds. A plugin verified on disk as 0x00080000 still read back
    // as 0x00000006.
    //
    // So the flag is read straight from the file. Four bytes at offset 8 of
    // the TES4 record, once per plugin.

    static bool ReadESLFlagFromDisk(const char* path)
    {
        if (!path || !path[0])
            return false;

        FILE* f = nullptr;
        fopen_s(&f, path, "rb");

        if (!f)
            return false;

        UInt8 header[12] = { 0 };
        size_t read = fread(header, 1, sizeof(header), f);
        fclose(f);

        if (read != sizeof(header))
            return false;

        // Must actually be a plugin.
        if (memcmp(header, "TES4", 4) != 0)
            return false;

        UInt32 flags =
            (UInt32)header[8]
            | ((UInt32)header[9] << 8)
            | ((UInt32)header[10] << 16)
            | ((UInt32)header[11] << 24);

        return (flags & kESLFlag) != 0;
    }

    // Results are cached by file pointer. ESLLoadPatch asks once per plugin,
    // but this can also be reached per form -- and disk I/O per form is exactly
    // the kind of per-record cost that caused the original load freeze.
    static std::unordered_map<ModEntry::Data*, bool> s_eslCache;

    void ClearESLCache()
    {
        s_eslCache.clear();

        _MESSAGE("[ESL] IsESLFile cache cleared.");
    }

    bool IsESLFile(ModEntry::Data* file)
    {
        if (!file)
            return false;

        auto it = s_eslCache.find(file);
        if (it != s_eslCache.end())
            return it->second;

        bool isESL = false;

        // filepath is relative to the Oblivion root, which is the working
        // directory at runtime.
        if (file->filepath[0])
            isESL = ReadESLFlagFromDisk(file->filepath);

        // Fall back to Data\<name> if filepath was empty or unusable.
        if (!isESL && file->name[0])
        {
            char path[0x120];
            sprintf_s(path, sizeof(path), "Data\\%s", file->name);
            isESL = ReadESLFlagFromDisk(path);
        }

        s_eslCache[file] = isESL;
        return isESL;
    }

    // ── ResolveFormID ───────────────────────────────────────────────────────────
    //
    // Vanilla behavior (from disassembly):
    //   unsigned int __cdecl TESForm_ResolveFormID(UInt32 *a1, Data *a2)
    //   {
    //       if (savegame flag 0x20000) { use SaveLoad_ResolveFormID; }
    //       else if (!*a1 || *a1 > 0x7FF)
    //       {
    //           MasterByIndex = TESFile_GetMasterByIndex(a2, HIBYTE(*a1) + 1);
    //           if (MasterByIndex)
    //               *a1 = (*a1 & 0xFFFFFF) | (GetFileIndex(MasterByIndex) << 24);
    //           else
    //               *a1 = (*a1 & 0xFFFFFF) | (GetFileIndex(a2) << 24);
    //       }
    //   }
    //
    // For an already-encoded ESL FormID (high byte 0xFE), HIBYTE+1 = 0xFF,
    // which doesn't correspond to a valid master — vanilla would silently
    // stamp the current file's own index into the high byte, corrupting the
    // FormID. So encoded IDs must skip vanilla resolution entirely.
    //
    // For a genuine cross-plugin reference to an ESL plugin, vanilla
    // resolution runs first (correctly resolving the raw modIndex), and only
    // afterward do we check whether that modIndex belongs to a registered
    // ESL and re-encode if so.

    void __cdecl ResolveFormID_Hook(
        UInt32* formID,
        ModEntry::Data* file
    )
    {
        if (!formID)
            return;

        if (ESLManager::Get().IsEncoded(*formID))
            return;

        UInt8 originalHighByte = (*formID >> 24) & 0xFF;

        g_ResolveFormID(formID, file);

        // Vanilla stamps the target file's index into the high byte. For an ESL
        // that index is 0xFE -- but bits 12-23, which say WHICH ESL, are left as
        // whatever the on-disk local ID had there. Since compacted local IDs are
        // always below 0x1000, those bits are always zero, so every ESL reference
        // would resolve to ESL 0 without this.
        if ((*formID >> 24) != 0xFE)
            return;

        // Work out which file vanilla resolved to, the same way it did.
        ModEntry::Data* target = nullptr;

        if (file && GetMasterByIndex)
            target = GetMasterByIndex(file, nullptr, originalHighByte + 1);

        if (!target)
            target = file;   // no such master: vanilla used the file itself

        UInt16 eslIndex = ESLManager::Get().GetESLIndexForFile(target);

        if (eslIndex == ESLManager::kInvalid)
        {
            _WARNING("[ESL] ResolveFormID: 0x%08X resolved to 0xFE but target "
                "'%s' is not a registered ESL",
                *formID, (target && target->name) ? target->name : "<null>");
            return;
        }

        UInt32 localID = *formID & 0x0FFF;

        if (!ESLManager::IsValidLocalID(localID))
        {
            _ERROR("[ESL] ResolveFormID: local ID 0x%X outside 0x%X-0x%X",
                localID, ESLManager::kMinLocalID, ESLManager::kMaxLocalID);
            return;
        }

        *formID = 0xFE000000 | ((UInt32)eslIndex << 12) | localID;
    }

    // ── SaveFormID ──────────────────────────────────────────────────────────────

    void __fastcall SaveFormID_Hook(
        void* thisPtr,
        void*,
        void* src,
        UInt32 size
    )
    {
        UInt32 count = size / 4;
        UInt32* ids = (UInt32*)src;

        ESLManager& manager = ESLManager::Get();

        for (UInt32 i = 0; i < count; i++)
        {
            UInt32& id = ids[i];
            if (!id)
                continue;

            if (manager.IsEncoded(id))
            {
                UInt16 eslIndex = manager.DecodeIndex(id);

                if (!manager.IsIndexValid(eslIndex))
                {
                    _WARNING(
                        "SaveFormID: Invalid ESL index %u for form %08X",
                        eslIndex,
                        id
                    );
                }
            }
        }

        g_SaveFormID(thisPtr, nullptr, src, size);
    }

    // ── SetFormID ───────────────────────────────────────────────────────────────
    //

    // Validation only -- this must NOT rewrite the ESL index.
    //
    // ResolveFormID_Hook has already produced the correct FormID by the time a
    // form reaches here, for both cases:
    //   - a record the ESL defines      -> its own ESL index
    //   - an override of another ESL    -> the MASTER's ESL index
    //
    // An earlier version re-stamped the index from g_currentLoadingESL. That
    // silently broke the second case: an override of an ESL record also arrives
    // with high byte 0xFE, so it was misattributed to the plugin doing the
    // overriding and stopped overriding anything.
    //
    // For the same reason there is no "was this resolved?" check here. A form
    // arriving as 0xFE000xxx is legitimately either an ESL 0 record or an
    // override of one, and nothing at this point distinguishes those from an
    // unresolved form. Comparing against the loading plugin's index gives a
    // false positive on every override of ESL 0.
    void __fastcall SetFormID_Hook(
        TESForm* form,
        void*,
        UInt32 newID,
        bool releaseOld
    )
    {
        if ((newID >> 24) == 0xFE)
        {
            UInt32 localID = newID & 0x0FFF;

            if (!ESLManager::IsValidLocalID(localID))
            {
                _ERROR("[ESL] SetFormID: form %08X has local ID 0x%X outside "
                    "0x%X-0x%X - plugin was not compacted correctly!",
                    newID, localID,
                    ESLManager::kMinLocalID, ESLManager::kMaxLocalID);
            }
        }

        g_SetFormID(form, nullptr, newID, releaseOld);
    }

    // ── TESDataHandler_IsFormIDCreated ──────────────────────────────────────────
    //
    // Vanilla tests only for high byte 0xFF. Across 39 call sites this function
    // is used as an "already absolute, skip remapping" guard -- and an ESL
    // FormID has exactly that property, since 0xFE forms carry their own index
    // and are never translated through the save's mod table.
    //
    // Without this, every one of those guards falls through to a table lookup
    // that uses the FormID as an INDEX. sub_459950 was one such site; hooking
    // it individually fixed some cases but left others (an actor's saved AI
    // package, for one), which is what makes the single shared hook the right
    // level to fix this at.
    //
    // CAVEAT worth remembering: not every caller means "already absolute". At
    // least one in TESSaveLoadGame_LoadGame uses it to gate whether a form gets
    // RESET, and returning true there changes behaviour for ESL forms rather
    // than just preserving them. If something ESL-specific misbehaves on load
    // that is not a missing-form problem, this is the first place to look.

    bool __stdcall IsFormIDCreated_Hook(UInt32 formID)
    {
        if ((formID >> 24) == 0xFE)
            return true;

        return g_IsFormIDCreated(formID);
    }

    // ── Saved ESL FormID translation ────────────────────────────────────────────
    //
    // Shared by both save-load remapping paths. A saved FormID carries the ESL
    // index its plugin had when the save was written; the cosave record lets us
    // translate that to the current one. Returns 0 for a plugin that is no
    // longer loaded, matching what vanilla does for any missing mod.

    static UInt32 TranslateSavedESLFormID(UInt32 formID)
    {
        ESLManager& manager = ESLManager::Get();

        UInt16 savedIndex = manager.DecodeIndex(formID);
        UInt16 eslIndex = manager.RemapSavedIndex(savedIndex);

        if (eslIndex == ESLManager::kInvalid ||
            !manager.IsESLIndexActive(eslIndex))
        {
            return 0;
        }

        if (eslIndex == savedIndex)
            return formID;

        return 0xFE000000
            | ((UInt32)eslIndex << 12)
            | (formID & 0x0FFF);
    }

    // ── SaveLoad_IRefToFormID (0x0045E0D0) ──────────────────────────────────────
    //
    // DIAGNOSTIC ONLY -- currently passes everything through unchanged.
    //
    // This is the save side of the iref table: it looks a FormID up in the
    // table at +0x74 and returns its index, appending a new entry if absent.
    // sub_459950 is the read side of the same table, index -> FormID.
    //
    // Why we are here: on load, three dropped ESL items resolved their base
    // form through index 0x1920/0x1921/0x1923 and got 0 back, while the table
    // count was 0x1926 -- so the indices are in range and the ENTRIES are zero.
    // The FormIDs themselves are present in the .ess, so something between the
    // write here and the read there is losing them.
    //
    // This logs what index each ESL FormID is assigned at save time, so the two
    // sides can be compared.

    UInt32 __fastcall IRefToFormID_Hook(
        void* saveLoad,
        void*,
        UInt32 formID
    )
    {
        UInt32 index = g_IRefToFormID(saveLoad, nullptr, formID);

        return index;
    }

    // ── SaveLoad_RemapSavedFormID (sub_459950) ──────────────────────────────────
    //
    // Vanilla:
    //
    //   if (TESDataHandler_IsFormIDCreated(formID)) return formID;
    //   table = this->+0x74;
    //   if (formID <= table->count) return table->entries[formID];
    //   return 0;
    //
    // IsFormIDCreated only tests for high byte 0xFF, so an ESL FormID falls
    // through to the table lookup -- where it is used as an INDEX. 0xFE003800
    // is far past any plausible count, so it returns 0 and the form is dropped
    // silently, with no error printed. That is what removes equipped ESL items
    // on load.
    //
    // ESL FormIDs are already absolute, exactly like created forms, so they get
    // the same passthrough 0xFF receives.

    UInt32 __fastcall RemapSavedFormID_Hook(
        void* saveLoad,
        void*,
        UInt32 formID
    )
    {
        if ((formID >> 24) == 0xFE)
            return TranslateSavedESLFormID(formID);

        return g_RemapSavedFormID(saveLoad, nullptr, formID);
    }

    // ── SaveLoad_ResolveFormID ──────────────────────────────────────────────────
    //
    // Vanilla:
    //
    //   modRefIDTable = this->modRefIDTable;
    //   if (!modRefIDTable || HIBYTE(a2) == 0xFF) return a2;
    //   if (HIBYTE(a2) >= this->numMods)          return 0;
    //   v3 = modRefIDTable[HIBYTE(a2)];
    //   if (v3 == 0xFF)                           return 0;
    //   return (a2 & 0xFFFFFF) + (v3 << 24);
    //
    // 0xFF (dynamic forms) is passed through untouched, but 0xFE is not. Since
    // numMods is the plugin count recorded in the save, 0xFE >= numMods is
    // effectively always true -- so WITHOUT this hook every ESL FormID read
    // back from a savegame resolves to 0, a null form.
    //
    // ESL indices are stable across load order by design, which is the whole
    // point of the persistent map, so they need no per-save remapping. They get
    // the same passthrough 0xFF receives.
    //
    // The one case that does need handling is a plugin the user has since
    // removed. Vanilla returns 0 for a mod no longer present, and we match that
    // rather than resolving to whatever form now occupies the slot.

    int __fastcall SaveLoadResolveFormID_Hook(
        void* saveLoad,
        void*,
        int formID
    )
    {
        if (((UInt32)formID >> 24) == 0xFE)
        {
            ESLManager& manager = ESLManager::Get();

            return (int)TranslateSavedESLFormID((UInt32)formID);
        }

        return g_SaveLoadResolveFormID(saveLoad, nullptr, formID);
    }

    // ── Iref table reader (sub_45E3D0) ──────────────────────────────────────────
    //
    // This function reads the iref table back from the save file, and it has
    // SaveLoad_ResolveFormID INLINED rather than calling it -- twice, once per
    // table. That is why hooking SaveLoad_ResolveFormID never affected these,
    // and why no SaveResolve lines appeared for the FormIDs that went missing.
    //
    // Vanilla, at 0x0045E48B and again at 0x0045E57B:
    //
    //   test edx, edx                  ; modRefIDTable
    //   jz   passthrough
    //   cmp  al, 0FFh                  ; high byte
    //   jz   passthrough
    //   cmp  al, [ebp+48h]             ; >= numMods?
    //   jnb  zero                      ; <-- 0xFE lands here
    //   ...
    //
    // 0xFE is always >= numMods, so every ESL FormID read back from the save
    // became 0 and was stored as 0 in the table. Later lookups by index then
    // returned 0, which is what removed dropped items, unequipped armour and
    // lost the AI package.

    UInt32 __stdcall ResolveSavedIref(void* saveLoad, UInt32 formID)
    {
        if ((formID >> 24) == 0xFE)
            return TranslateSavedESLFormID(formID);

        // Anything else gets vanilla's own logic, via the trampoline.
        return (UInt32)g_SaveLoadResolveFormID(saveLoad, nullptr, (int)formID);
    }

    // ecx = raw FormID, ebp = TESSaveLoad. Only eax is live on exit; ecx and
    // edx are both reloaded before their next use, and a __stdcall callee
    // preserves ebx/esi/edi/ebp -- so no pushad is needed.
    static const UInt32 kIrefBlock1Return = 0x0045E4B9;
    static const UInt32 kIrefBlock2Return = 0x0045E5A9;

    __declspec(naked) void IrefBlock1_Stub()
    {
        __asm
        {
            push    ecx             // formID
            push    ebp             // saveLoad
            call    ResolveSavedIref
            jmp     kIrefBlock1Return
        }
    }

    __declspec(naked) void IrefBlock2_Stub()
    {
        __asm
        {
            push    ecx             // formID
            push    ebp             // saveLoad
            call    ResolveSavedIref
            jmp     kIrefBlock2Return
        }
    }

    static bool WriteJump(UInt32 address, void* target, UInt32 patchSize)
    {
        if (patchSize < 5)
            return false;

        DWORD oldProtect;
        VirtualProtect((void*)address, patchSize, PAGE_EXECUTE_READWRITE, &oldProtect);

        *(UInt8*)address = 0xE9;
        *(UInt32*)(address + 1) = (UInt32)target - (address + 5);

        for (UInt32 i = 5; i < patchSize; i++)
            *(UInt8*)(address + i) = 0x90;

        VirtualProtect((void*)address, patchSize, oldProtect, &oldProtect);
        return true;
    }

    // ── Hook installation ──────────────────────────────────────────────────────
    //
    // Note: the LoadFile hook was removed. Registration now happens in
    // ESLLoadPatch::AppendFile, which sees every plugin before any of them
    // load -- so ordering between an ESL and its dependents no longer matters.

    bool InstallHooks()
    {
        g_ResolveFormID =
            (tResolveFormID)ESLDetour::WriteDetour(
                (void*)0x0046BB20,
                ResolveFormID_Hook,
                14
            );

        if (!g_ResolveFormID)
        {
            _ERROR("ResolveFormID hook failed!");
            return false;
        }

        g_SaveFormID =
            (tSaveFormID)ESLDetour::WriteDetour(
                (void*)0x0045F7A0,
                SaveFormID_Hook,
                13
            );

        if (!g_SaveFormID)
        {
            _ERROR("SaveFormID hook failed!");
            return false;
        }

        g_SetFormID =
            (tSetFormID)ESLDetour::WriteDetour(
                (void*)0x0046C300,
                SetFormID_Hook,
                11
            );

        if (!g_SetFormID)
        {
            _ERROR("SetFormID hook failed!");
            return false;
        }

        // 0x00452180, 8 bytes. Verified against the disassembly:
        //   00452180  8B 51 4C     mov  edx, [ecx+4Ch]
        //   00452183  56           push esi
        //   00452184  8B 74 24 08  mov  esi, [esp+8]
        // Three whole instructions, no branches (first jump is at +0x0F), and
        // the esp-relative access is safe to relocate since the trampoline is
        // entered by a normal CALL and sees the same stack layout.
        g_SaveLoadResolveFormID =
            (tSaveLoadResolveFormID)ESLDetour::WriteDetour(
                (void*)0x00452180,
                SaveLoadResolveFormID_Hook,
                8
            );

        if (!g_SaveLoadResolveFormID)
        {
            _ERROR("SaveLoad_ResolveFormID hook failed!");
            return false;
        }

        // 0x00459950, 5 bytes:
        //   00459950  56           push esi
        //   00459951  8B 74 24 08  mov  esi, [esp+8]
        // Two whole instructions, no branches.
        g_RemapSavedFormID =
            (tRemapSavedFormID)ESLDetour::WriteDetour(
                (void*)0x00459950,
                RemapSavedFormID_Hook,
                5
            );

        if (!g_RemapSavedFormID)
        {
            _ERROR("RemapSavedFormID hook failed!");
            return false;
        }

        // 0x00446B80, 8 bytes:
        //   00446B80  81 7C 24 04 00 00 00 FF  cmp [esp+4], 0FF000000h
        // One whole instruction, no branches. Vanilla is only 14 bytes total:
        //   cmp [esp+4], 0FF000000h / sbb eax, eax / add eax, 1 / retn 4
        // which returns true for formID >= 0xFF000000.
        g_IsFormIDCreated =
            (tIsFormIDCreated)ESLDetour::WriteDetour(
                (void*)0x00446B80,
                IsFormIDCreated_Hook,
                8
            );

        if (!g_IsFormIDCreated)
        {
            _ERROR("IsFormIDCreated hook failed!");
            return false;
        }

        // 0x0045E0D0, 5 bytes:
        //   0045E0D0  53           push ebx
        //   0045E0D1  8B 5C 24 08  mov  ebx, [esp+8]
        // Two whole instructions, no branches.
        g_IRefToFormID =
            (tIRefToFormID)ESLDetour::WriteDetour(
                (void*)0x0045E0D0,
                IRefToFormID_Hook,
                5
            );

        if (!g_IRefToFormID)
        {
            _ERROR("IRefToFormID hook failed!");
            return false;
        }

        // The two inlined resolve blocks in sub_45E3D0, 46 bytes each.
        //   Block 1: 0x0045E48B -> 0x0045E4B9  (table at +0x74)
        //   Block 2: 0x0045E57B -> 0x0045E5A9  (table at +0x78)
        if (!WriteJump(0x0045E48B, IrefBlock1_Stub, 0x2E))
        {
            _ERROR("Iref block 1 patch failed!");
            return false;
        }

        if (!WriteJump(0x0045E57B, IrefBlock2_Stub, 0x2E))
        {
            _ERROR("Iref block 2 patch failed!");
            return false;
        }

        return true;
    }

}