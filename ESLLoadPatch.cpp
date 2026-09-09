#include "ESLLoadPatch.h"
#include "ESLManager.h"
#include "ESLHooks.h"
#include "obse/GameData.h"
#include "obse/GameAPI.h"
#include <windows.h>
#include <vector>

namespace ESLLoadPatch {

    // ── Engine functions we call directly ───────────────────────────────────────

    // __thiscall, one stack arg, retn 4.
    // Writes the index into the high byte of nextFormID (+0x3D8) and stores it
    // directly at +0x400 -- which is the field TESFile_GetFileIndex reads.
    typedef void(__fastcall* tSetFileIndex)(void* file, void* edx, UInt8 index);

    // __thiscall, two stack args, retn 8.
    typedef bool(__fastcall* tOpenBSFileWrapper)(void* file, void* edx, int a2, int a3);

    tSetFileIndex      SetFileIndex = (tSetFileIndex)0x0044FB20;
    tOpenBSFileWrapper OpenBSFileWrapper = (tOpenBSFileWrapper)0x00451A40;

    // TESDataHandler::LoadFile -- the same function ESLHooks detours, so calls
    // made here still pass through LoadFile_Hook. That is intended.
    typedef int(__fastcall* tLoadFile)(void* dh, void* edx, void* file, char flag);
    tLoadFile LoadFile = (tLoadFile)0x0044F0C0;

    // ── ESL file list ───────────────────────────────────────────────────────────
    //
    // ESLs never enter modsByID, so they need somewhere to live. Each entry
    // also records the numLoadedMods value at the moment it was skipped --
    // the slot it WOULD have taken. That is what preserves the user's load
    // order: the ESL loads immediately after the normal plugin occupying the
    // slot before it, not in a bulk pass at the end.
    //
    // Without this, all ESLs would load last, ESL overrides would always win
    // regardless of position, and any normal plugin mastering off an ESL would
    // load before its master existed.

    struct ESLEntry
    {
        ModEntry::Data* file;
        UInt32          position;   // numLoadedMods when skipped
        bool            loaded;     // handled by the interleaved pass already
    };

    static std::vector<ESLEntry> s_eslFiles;

    // DIAGNOSTIC: how many times TESDataHandler_LoadFiles has begun, and how
    // many ESL load passes have run. If a plugin is loaded twice in one session
    // without an intervening teardown, its records exist twice -- two forms
    // claiming one FormID, one of them orphaned -- which would corrupt any list
    // holding them.
    static UInt32 s_loadRunCount = 0;

    // Set while an ESL is being loaded. This replaces modIndex as the key for
    // "which ESL does this record belong to" -- once every ESL has file index
    // 0xFE, the high byte no longer distinguishes them.
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



    // ── Append handler (sites 1 and 2) ──────────────────────────────────────────
    //
    // Replaces the vanilla sequence:
    //     modsByID[numLoadedMods] = file;
    //     TESFile_SetFileIndex(file, (UInt8)numLoadedMods);
    //     numLoadedMods++;
    //     if (numLoadedMods >= 0xFF) Fatal("Too many selected files");
    //
    // Note the vanilla file index is the LOW BYTE of numLoadedMods, so simply
    // deleting the >= 0xFF check would wrap index 256 to 0 and alias every
    // record onto Oblivion.esm. The array is also physically 255 entries --
    // modsByID ends at 0xCD0, exactly where the flags at 0xCD0/0xCD1/0xCD7
    // begin -- so a 256th write would corrupt them.
    //
    // ESLs sidestep both by never being appended at all.

    void __stdcall AppendFile(DataHandler* dh, ModEntry::Data* file)
    {
        if (!dh || !file)
            return;

        // Start of a fresh TESDataHandler_LoadFiles run: numLoadedMods has
        // just been zeroed, so any state keyed on the previous run's files is
        // stale and must go.
        //
        // Note this does NOT dereference the old entries. An earlier version
        // cleared a marker flag on each file->flags here, which is a
        // use-after-free if those ModEntry::Data objects have been released.
        // "Already loaded" is tracked on our own ESLEntry instead.
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

            // Register here rather than at load time. Every plugin passes
            // through this function before any of them load, so a dependent
            // that resolves references to an ESL always finds it registered,
            // whatever the relative order.
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

            _MESSAGE("[ESL] Deferred '%s' -> ESL %u (loads at position %u)",
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

    // ── ESL loading ─────────────────────────────────────────────────────────────
    //
    // Vanilla's load loop iterates modsByID, so a file kept out of that array is
    // never loaded at all -- skipping the append frees a slot AND removes the
    // plugin from the pipeline. These run the same two steps vanilla does
    // (OpenBSFileWrapper, then LoadFile) for each deferred ESL.

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

    // Called from the tail of vanilla's load loop, once per normal plugin.
    //
    // `justLoadedIndex` is the loop counter BEFORE its increment -- the index of
    // the plugin just loaded. An ESL's recorded position is numLoadedMods at the
    // time it was skipped, i.e. the count of normal plugins appended ahead of
    // it. After loading index i the count is i + 1, so the +1 here is required:
    // comparing against the raw index loads every ESL one plugin too late, and
    // any normal plugin mastering an ESL then loads before its master exists.
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

    // Cleanup pass, after vanilla's loop. Picks up ESLs positioned past the
    // last normal plugin, plus the position-0 case (an ESL ahead of every
    // normal plugin), which the loop tail never reaches.
    //
    // Runs before the 0xCD7 loading flag is cleared at 0x0044F785.
    void __stdcall LoadRemainingESLs(DataHandler* dh)
    {
        if (!dh)
            return;

        for (ESLEntry& entry : s_eslFiles)
            LoadOne(dh, entry);
    }

    // ── Record header FormID stamping (site 5) ──────────────────────────────────
    //
    // Replaces the tail of TESFile_LoadRecordHeader, 0x00450100 - 0x0045017E.
    //
    // This is where a record's own FormID gets its load order byte, and it is
    // the ONLY place that knows whether the record belongs to the file or to
    // one of its masters:
    //
    //     movzx eax, byte ptr [esi+24Bh]    ; original high byte
    //     add   eax, 1
    //     call  TESFile_GetMasterByIndex
    //     test  eax, eax
    //     jz    ...                         ; null -> the file's OWN record
    //     movzx edx, byte ptr [eax+400h]    ; else -> the MASTER's index
    //
    // Every earlier attempt at fixing identities failed for want of this test.
    // SetFormID_Hook and LoadFormRecord_Hook both run after the stamp, when the
    // original high byte is gone, so an override of another ESL is
    // indistinguishable from the plugin's own record -- which is what kept
    // re-stamping Alternative Start's records as ESL 1 while the Unofficial
    // Patch was loading.

    void __stdcall StampRecordFormID(ModEntry::Data* file, ModEntry::Data* master)
    {
        if (!file)
            return;

        UInt32 raw = file->currentRecordInfo.recordID;
        UInt32 local = raw & 0x00FFFFFF;

        // Vanilla only applies the builtin check when there is no master.
        if (!master)
        {
            bool wasDynamic = (raw & 0xFF000000) == 0xFF000000;

            // TESForm_IsFormIDBuiltin: non-zero and <= 0x7FF. Hardcoded engine
            // forms keep their ID; vanilla clears the high byte and stops.
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
            // Vanilla behaviour: stamp the target's load order byte.
            file->currentRecordInfo.recordID =
                ((UInt32) * ((UInt8*)target + 0x400) << 24) | local;
        }
    }

    // ── Naked stubs ─────────────────────────────────────────────────────────────
    //
    // These are JMP patches over a block of engine code, not trampoline detours:
    // the replaced instructions are discarded rather than relocated, and control
    // returns by an explicit jump. pushad/popad protects the surrounding loop's
    // registers (esi, edi, ebx and ebp are all live across these sites).

    static const UInt32 kSite1Return = 0x0044F64E;
    static const UInt32 kSite2Return = 0x0044F6C9;
    static const UInt32 kSite3Return = 0x0044F785;
    static const UInt32 kSite4Return = 0x0044F77E;

    // All vanilla exits from the replaced block converge here, where edi/esi/ecx
    // are popped and al is set to 1. Jumping to it keeps the epilogue intact.
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

    // Loop tail: replaces
    //     add  edi, 1
    //     cmp  edi, [esi+8D0h]
    // and returns to the "jb short loc_44F735" that follows.
    //
    // The add/cmp are re-run AFTER popad, not before: popad does not restore
    // EFLAGS, so the flags the jb depends on must be set last.
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

    // ── Installation ────────────────────────────────────────────────────────────

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