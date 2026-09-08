#include "ESLHooks.h"
#include "ESLManager.h"
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
    tLoadFile      g_LoadFile = nullptr;

    constexpr UInt32 kESLFlag = 0x00080000;

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

    // DataHandler::modsByID[0xFF] is indexed by modIndex — this is the correct
    // way to resolve a ModEntry::Data* back to its runtime modIndex.
    // (file->idx is NOT the load-order index — it's the master count, per the
    // OBSE header comment. Do not use it for this purpose.)
    static UInt8 GetModIndex(ModEntry::Data* file)
    {
        if (!g_dataHandler || !*g_dataHandler)
            return 0xFF;

        DataHandler* dh = *g_dataHandler;

        for (UInt32 i = 0; i < dh->numLoadedMods; i++)
        {
            if (dh->modsByID[i] == file)
                return (UInt8)i;
        }

        _WARNING("[ESL] GetModIndex: '%s' not found in modsByID",
            file->name ? file->name : "<null>");

        return 0xFF;
    }

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

    // Results are cached by file pointer. LoadFile_Hook only asks once per
    // plugin, but SetFormID_Hook's lazy fallback can ask on any form -- and
    // doing disk I/O per form is exactly the kind of per-record cost that
    // caused the original load freeze.
    static std::unordered_map<ModEntry::Data*, bool> s_eslCache;

    static bool IsESLFile(ModEntry::Data* file)
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

        g_ResolveFormID(formID, file);

        UInt8 modIndex = (*formID >> 24) & 0xFF;
        ESLManager& manager = ESLManager::Get();

        if (manager.HasRuntimeMapping(modIndex))
        {
            UInt16 eslIndex = manager.GetRuntimeESLIndex(modIndex);
            UInt32 localID = *formID & 0x00FFFFFF;

            // Reject BOTH ends of the range. An ID below 0x800 means the
            // plugin was not compacted correctly, and encoding it anyway
            // would produce a form the engine cannot resolve.
            if (!ESLManager::IsValidLocalID(localID))
            {
                _ERROR(
                    "ResolveFormID: ESL form 0x%08X has localID 0x%X outside "
                    "the valid 0x%X-0x%X range!",
                    *formID, localID,
                    ESLManager::kMinLocalID, ESLManager::kMaxLocalID
                );
                return;
            }

            *formID = 0xFE000000 | ((UInt32)eslIndex << 12) | (localID & 0x0FFF);
        }
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
    // Registration normally happens in LoadFile_Hook before any records from
    // that file are processed. This lazy fallback only fires if SetFormID is
    // somehow reached before that registration completes.

    void __fastcall SetFormID_Hook(
        TESForm* form,
        void*,
        UInt32 newID,
        bool releaseOld
    )
    {
        if (!form)
        {
            g_SetFormID(form, nullptr, newID, releaseOld);
            return;
        }

        UInt8 modIndex = (newID >> 24) & 0xFF;
        ESLManager& manager = ESLManager::Get();

        if (!manager.HasRuntimeMapping(modIndex))
        {
            if (!g_dataHandler || !*g_dataHandler)
            {
                g_SetFormID(form, nullptr, newID, releaseOld);
                return;
            }

            DataHandler* dh = *g_dataHandler;

            if (modIndex < dh->numLoadedMods)
            {
                ModEntry::Data* file = dh->modsByID[modIndex];

                if (file && IsESLFile(file))
                {
                    UInt16 eslIndex = manager.GetOrRegisterESLIndex(file->name);

                    if (eslIndex != ESLManager::kInvalid)
                        manager.RegisterRuntimeMapping(modIndex, eslIndex);
                }
            }
        }

        if (manager.HasRuntimeMapping(modIndex))
        {
            UInt16 eslIndex = manager.GetRuntimeESLIndex(modIndex);
            UInt32 localID = newID & 0x00FFFFFF;

            if (!ESLManager::IsValidLocalID(localID))
            {
                _ERROR(
                    "[ESL] SetFormID: form %08X in '%s' has localID 0x%X outside "
                    "the valid 0x%X-0x%X range - plugin was not compacted "
                    "correctly!",
                    newID,
                    (*g_dataHandler)->GetNthModName(modIndex),
                    localID,
                    ESLManager::kMinLocalID,
                    ESLManager::kMaxLocalID
                );

                // Pass through unmodified - better to load into the wrong slot
                // than to silently alias onto a different form.
                g_SetFormID(form, nullptr, newID, releaseOld);
                return;
            }

            newID = 0xFE000000 | ((UInt32)eslIndex << 12) | (localID & 0x0FFF);
        }

        g_SetFormID(form, nullptr, newID, releaseOld);
    }

    // ── LoadFile ────────────────────────────────────────────────────────────────
    //
    // Primary ESL detection/registration point. Runs vanilla load first so
    // the file is present in DataHandler's modsByID before we scan for its
    // modIndex.

    int __fastcall LoadFile_Hook(
        void* thisPtr,
        void*,
        void* tesFile,
        char flag
    )
    {
        int result = g_LoadFile(thisPtr, nullptr, tesFile, flag);

        if (!result || !tesFile)
            return result;

        ModEntry::Data* file = (ModEntry::Data*)tesFile;

        if (!IsESLFile(file))
            return result;

        ESLManager& manager = ESLManager::Get();

        UInt8 modIndex = GetModIndex(file);

        if (modIndex == 0xFF)
        {
            _ERROR("[ESL] LoadFile: could not determine modIndex for '%s', skipping",
                file->name);
            return result;
        }

        UInt16 eslIndex = manager.GetOrRegisterESLIndex(file->name);

        if (eslIndex == ESLManager::kInvalid)
        {
            _ERROR("[ESL] LoadFile: registration failed for '%s'", file->name);
            return result;
        }

        bool alreadyMapped = manager.HasRuntimeMapping(modIndex);

        if (!alreadyMapped)
            manager.RegisterRuntimeMapping(modIndex, eslIndex);

        _MESSAGE("[ESL] Loaded: %s (modIndex %02X -> ESL slot %u)%s",
            file->name, modIndex, eslIndex,
            alreadyMapped ? "  [pre-registered during record load]" : "");

        return result;
    }

    // ── Hook installation ──────────────────────────────────────────────────────
    //
    // Note: InitializeFormFromRecord and LoadFormID hooks were removed.
    // InitializeFormFromRecord fires per-record (tens of thousands of times
    // per load) and duplicated detection that LoadFile_Hook already does
    // once per file — this was the primary cause of the load freeze.
    // LoadFormID was a pure passthrough with no logic.

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

        g_LoadFile =
            (tLoadFile)ESLDetour::WriteDetour(
                (void*)0x0044F0C0,
                LoadFile_Hook,
                6
            );

        if (!g_LoadFile)
        {
            _ERROR("LoadFile hook failed!");
            return false;
        }

        return true;
    }

}