#include "ESLLoadPatch.h"
#include "ESLManager.h"
#include "ESLHooks.h"
#include "obse/GameData.h"
#include "obse/GameAPI.h"
#include <windows.h>
#include <vector>

namespace ESLLoadPatch {

    typedef void(__fastcall* tSetFileIndex)(void* file, void* edx, UInt8 index);

    typedef bool(__fastcall* tOpenBSFileWrapper)(void* file, void* edx, int a2, int a3);

    tSetFileIndex      SetFileIndex = (tSetFileIndex)0x0044FB20;
    tOpenBSFileWrapper OpenBSFileWrapper = (tOpenBSFileWrapper)0x00451A40;

    typedef int(__fastcall* tLoadFile)(void* dh, void* edx, void* file, char flag);
    tLoadFile LoadFile = (tLoadFile)0x0044F0C0;

    struct ESLEntry
    {
        ModEntry::Data* file;
        UInt32          position;
        bool            loaded;
    };

    static std::vector<ESLEntry> s_eslFiles;

    static UInt32 s_loadRunCount = 0;

    ModEntry::Data* g_currentLoadingESL = nullptr;

    UInt32 GetESLCount() { return (UInt32)s_eslFiles.size(); }

    std::vector<ModEntry::Data*> GetESLFiles()
    {
        std::vector<ModEntry::Data*> out;
        out.reserve(s_eslFiles.size());

        for (const ESLEntry& entry : s_eslFiles)
            out.push_back(entry.file);

        return out;
    }

    void ClearFileList()
    {
        s_eslFiles.clear();
        g_currentLoadingESL = nullptr;

        _MESSAGE("[ESL] Deferred file list cleared.");
    }

    void __stdcall AppendFile(DataHandler* dh, ModEntry::Data* file)
    {
        if (!dh || !file)
            return;

        if (dh->numLoadedMods == 0 && !s_eslFiles.empty())
        {
            UInt32 dropped = (UInt32)s_eslFiles.size();

            s_eslFiles.clear();
            g_currentLoadingESL = nullptr;

            ESLManager::Get().ClearRuntimeState();
            ESLHooks::ClearESLCache();

            _MESSAGE("[ESL] === LoadFiles run #%u begins; runtime state reset "
                "(%u ESL entries dropped) ===",
                ++s_loadRunCount, dropped);
        }

        if (ESLHooks::IsESLFile(file))
        {
            SetFileIndex(file, nullptr, 0xFE);

            ESLManager& manager = ESLManager::Get();

            UInt16 eslIndex = manager.GetOrRegisterESLIndex(file->name);

            if (eslIndex == ESLManager::kInvalid)
            {
                _ERROR("[ESL] Cannot register '%s'; ESL limit reached",
                    file->name);
                return;
            }

            manager.RegisterFile(file, eslIndex);

            ESLEntry entry;
            entry.file = file;
            entry.position = dh->numLoadedMods;
            entry.loaded = false;

            s_eslFiles.push_back(entry);

            _MESSAGE("[ESL] Deferred '%s' -> ESL %03X (loads at position %u)",
                file->name, eslIndex, entry.position);
            return;
        }

        UInt32 index = dh->numLoadedMods;

        if (index >= 0xFF)
        {
            _ERROR("[ESL] Load order full at %u; cannot add '%s'",
                index, file->name);
            return;
        }

        dh->modsByID[index] = file;
        dh->numLoadedMods = index + 1;

        SetFileIndex(file, nullptr, (UInt8)index);
    }

    void ResetLoadedFlags()
    {
        for (ESLEntry& entry : s_eslFiles)
            entry.loaded = false;

        g_currentLoadingESL = nullptr;

        _MESSAGE("[ESL] Reset loaded flags on %u deferred ESL(s).",
            (UInt32)s_eslFiles.size());
    }

    static std::vector<ModEntry*> s_unlinked;

    static bool IsOurESLFile(ModEntry::Data* data)
    {
        if (!data)
            return false;

        for (const ESLEntry& entry : s_eslFiles)
        {
            if (entry.file == data)
                return true;
        }

        return false;
    }

    void UnlinkESLsFromModList()
    {
        if (!g_dataHandler || !*g_dataHandler || s_eslFiles.empty())
            return;

        DataHandler* dh = *g_dataHandler;

        ModEntry* head = &dh->modList;
        UInt32 removed = 0;

        while (IsOurESLFile(head->data) && head->next)
        {
            ModEntry* orphan = head->next;

            head->data = orphan->data;
            head->next = orphan->next;

            s_unlinked.push_back(orphan);
            removed++;
        }

        for (ModEntry* prev = head; prev && prev->next; )
        {
            ModEntry* node = prev->next;

            if (IsOurESLFile(node->data))
            {
                prev->next = node->next;

                s_unlinked.push_back(node);
                removed++;
            }
            else
            {
                prev = node;
            }
        }

        //_MESSAGE("[ESL] Unlinked %u ESL file(s) from modList.", removed);
    }

    void RelinkESLsToModList()
    {
        if (!g_dataHandler || !*g_dataHandler || s_unlinked.empty())
            return;

        DataHandler* dh = *g_dataHandler;

        ModEntry* head = &dh->modList;

        for (ModEntry* node : s_unlinked)
        {
            node->next = head->next;
            head->next = node;
        }

        _MESSAGE("[ESL] Relinked %u ESL file(s) into modList.",
            (UInt32)s_unlinked.size());

        s_unlinked.clear();
    }

    static void LoadOne(DataHandler* dh, ESLEntry& entry)
    {
        if (entry.loaded)
            return;

        entry.loaded = true;

        if (!OpenBSFileWrapper(entry.file, nullptr, 0, 0))
        {
            _ERROR("[ESL] OpenBSFile failed for '%s'", entry.file->name);
            return;
        }

        g_currentLoadingESL = entry.file;

        int result = LoadFile(dh, nullptr, entry.file, 0);

        g_currentLoadingESL = nullptr;

        if (!result)
            _ERROR("[ESL] LoadFile failed for '%s'", entry.file->name);
        else
            _MESSAGE("[ESL] Loaded '%s' at position %u (run #%u, file %p)",
                entry.file->name, entry.position, s_loadRunCount, entry.file);
    }

    void __stdcall LoadESLsAt(DataHandler* dh, UInt32 justLoadedIndex)
    {
        if (!dh)
            return;

        UInt32 loaded = justLoadedIndex + 1;

        for (ESLEntry& entry : s_eslFiles)
        {
            if (entry.position == loaded)
                LoadOne(dh, entry);
        }
    }

    void __stdcall LoadRemainingESLs(DataHandler* dh)
    {
        if (!dh)
            return;

        for (ESLEntry& entry : s_eslFiles)
            LoadOne(dh, entry);


        UnlinkESLsFromModList();
    }

    void __stdcall StampRecordFormID(ModEntry::Data* file, ModEntry::Data* master)
    {
        if (!file)
            return;

        UInt32 raw = file->currentRecordInfo.recordID;
        UInt32 local = raw & 0x00FFFFFF;

        if (!master)
        {
            bool wasDynamic = (raw & 0xFF000000) == 0xFF000000;

            if (!wasDynamic && local != 0 && local <= 0x7FF)
            {
                *((UInt8*)file + 0x24B) = 0;
                return;
            }
        }

        ModEntry::Data* target = master ? master : file;

        UInt16 eslIndex = ESLManager::Get().GetESLIndexForFile(target);

        if (eslIndex != ESLManager::kInvalid)
        {
            file->currentRecordInfo.recordID =
                0xFE000000
                | ((UInt32)eslIndex << 12)
                | (local & 0x0FFF);
        }
        else
        {
            file->currentRecordInfo.recordID =
                ((UInt32) * ((UInt8*)target + 0x400) << 24) | local;
        }
    }

    tReloadAllFiles g_ReloadAllFiles = nullptr;
    tGetModCount    g_GetModCount = nullptr;
    tGetNthMod      g_GetNthMod = nullptr;

    static bool s_reloadPassActive = false;
    static UInt32 s_reloadBaseCount = 0;

    UInt32 __fastcall GetModCount_Hook(void* dataHandler, void*)
    {
        UInt32 count = g_GetModCount(dataHandler, nullptr);

        if (!s_reloadPassActive)
            return count;

        s_reloadBaseCount = count;

        return count + (UInt32)s_eslFiles.size();
    }

    void* __fastcall GetNthMod_Hook(void* dataHandler, void*, UInt32 index)
    {
        if (!s_reloadPassActive || index < s_reloadBaseCount)
        {
            g_currentLoadingESL = nullptr;

            return g_GetNthMod(dataHandler, nullptr, index);
        }

        UInt32 eslIndex = index - s_reloadBaseCount;

        if (eslIndex >= s_eslFiles.size())
        {
            g_currentLoadingESL = nullptr;
            return nullptr;
        }

        ModEntry::Data* file = s_eslFiles[eslIndex].file;

        g_currentLoadingESL = file;

        return file;
    }

    int __fastcall ReloadAllFiles_Hook(void* thisPtr, void*)
    {
        s_reloadPassActive = true;
        s_reloadBaseCount = 0;

        _MESSAGE("[ESL] Reload pass beginning (%u ESL(s) to include)",
            (UInt32)s_eslFiles.size());

        int result = g_ReloadAllFiles(thisPtr, nullptr);

        s_reloadPassActive = false;
        g_currentLoadingESL = nullptr;

        return result;
    }

    bool InstallReloadHook()
    {
        g_ReloadAllFiles =
            (tReloadAllFiles)ESLHooks::ESLDetour::WriteDetour(
                (void*)0x0045EC50,
                ReloadAllFiles_Hook,
                8
            );

        if (!g_ReloadAllFiles)
        {
            _ERROR("[ESL] ReloadAllFiles hook failed!");
            return false;
        }

        g_GetModCount =
            (tGetModCount)ESLHooks::ESLDetour::WriteDetour(
                (void*)0x00446B10,
                GetModCount_Hook,
                6
            );

        if (!g_GetModCount)
        {
            _ERROR("[ESL] GetModCount hook failed!");
            return false;
        }

        g_GetNthMod =
            (tGetNthMod)ESLHooks::ESLDetour::WriteDetour(
                (void*)0x00446B20,
                GetNthMod_Hook,
                6
            );

        if (!g_GetNthMod)
        {
            _ERROR("[ESL] GetNthMod hook failed!");
            return false;
        }

        _MESSAGE("[ESL] Reload hooks installed.");
        return true;
    }


    static const UInt32 kSite1Return = 0x0044F64E;
    static const UInt32 kSite2Return = 0x0044F6C9;
    static const UInt32 kSite3Return = 0x0044F785;
    static const UInt32 kSite4Return = 0x0044F77E;

    static const UInt32 kSite5Return = 0x0045017E;

    __declspec(naked) void Site1_Stub()
    {
        __asm
        {
            pushad
            push    edi             // file
            push    esi             // DataHandler
            call    AppendFile
            popad
            jmp     kSite1Return
        }
    }

    __declspec(naked) void Site2_Stub()
    {
        __asm
        {
            pushad
            push    eax             // file
            push    esi             // DataHandler
            call    AppendFile
            popad
            jmp     kSite2Return
        }
    }

    __declspec(naked) void Site4_Stub()
    {
        __asm
        {
            pushad
            push    edi             // index just loaded (pre-increment)
            push    esi             // DataHandler
            call    LoadESLsAt
            popad

            add     edi, 1
            cmp     edi, [esi + 0x8D0]
            jmp     kSite4Return
        }
    }

    __declspec(naked) void Site3_Stub()
    {
        __asm
        {
            pushad
            push    esi             // DataHandler
            call    LoadRemainingESLs
            popad

            // Re-run the two instructions this patch displaced:
            //   lea  edi, [esi+44h]
            //   test edi, edi
            lea     edi, [esi + 0x44]
            test    edi, edi
            jmp     kSite3Return
        }
    }

    // eax = master file pointer or null, esi = the file being read.
    __declspec(naked) void Site5_Stub()
    {
        __asm
        {
            pushad
            push    eax             // master (may be null)
            push    esi             // file
            call    StampRecordFormID
            popad
            jmp     kSite5Return
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

    bool InstallPatches()
    {
        // Site 1: inactive-file append. 0x0044F612 - 0x0044F64E, 60 bytes.
        // esi = DataHandler, edi = file.
        if (!WriteJump(0x0044F612, Site1_Stub, 0x3C))
            return false;

        // Site 2: active-file append. 0x0044F689 - 0x0044F6C9, 64 bytes.
        // esi = DataHandler, eax = file. Starts after vanilla's null check.
        if (!WriteJump(0x0044F689, Site2_Stub, 0x40))
            return false;

        // Site 4: load loop tail. 0x0044F775 - 0x0044F77E, 9 bytes, covering
        // "add edi,1" + "cmp edi,[esi+8D0h]". Interleaves ESLs at their
        // recorded positions so user load order is preserved.
        if (!WriteJump(0x0044F775, Site4_Stub, 9))
            return false;

        // Site 3: cleanup pass. 0x0044F780, 5 bytes, covering
        // "lea edi,[esi+44h]" + "test edi,edi". Catches ESLs positioned past
        // the last normal plugin. Placed before the 0xCD7 loading flag is
        // cleared at 0x0044F785.
        if (!WriteJump(0x0044F780, Site3_Stub, 5))
            return false;

        // Site 5: record header FormID stamping.
        // 0x00450100 - 0x0045017E, 126 bytes. eax = master or null, esi = file.
        if (!WriteJump(0x00450100, Site5_Stub, 0x7E))
            return false;

        _MESSAGE("[ESL] Load order patches installed.");
        return true;
    }
}