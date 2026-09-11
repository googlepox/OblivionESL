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
    tIRefToFormID g_IRefToFormID = nullptr;

    constexpr UInt32 kESLFlag = 0x00080000;
    constexpr UInt32 kESLFlagxEdit = 0x00000200;

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

            BYTE* trampoline = (BYTE*)VirtualAlloc(
                nullptr,
                patchSize + 5,
                MEM_COMMIT | MEM_RESERVE,
                PAGE_EXECUTE_READWRITE
            );

            if (!trampoline)
                return nullptr;

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

        if (memcmp(header, "TES4", 4) != 0)
            return false;

        UInt32 flags =
            (UInt32)header[8]
            | ((UInt32)header[9] << 8)
            | ((UInt32)header[10] << 16)
            | ((UInt32)header[11] << 24);


        return ((flags & kESLFlag) != 0) || ((flags & kESLFlagxEdit) != 0);
    }

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

        if (file->filepath[0])
            isESL = ReadESLFlagFromDisk(file->filepath);

        if (!isESL && file->name[0])
        {
            char path[0x120];
            sprintf_s(path, sizeof(path), "Data\\%s", file->name);
            isESL = ReadESLFlagFromDisk(path);
        }

        s_eslCache[file] = isESL;
        return isESL;
    }

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

        if ((*formID >> 24) != 0xFE)
            return;

        ModEntry::Data* target = nullptr;

        if (file && GetMasterByIndex)
            target = GetMasterByIndex(file, nullptr, originalHighByte + 1);

        if (!target)
            target = file;

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

    UInt32 __fastcall IRefToFormID_Hook(
        void* saveLoad,
        void*,
        UInt32 formID
    )
    {
        UInt32 index = g_IRefToFormID(saveLoad, nullptr, formID);

        return index;
    }

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

    UInt32 __stdcall ResolveSavedIref(void* saveLoad, UInt32 formID)
    {
        if ((formID >> 24) == 0xFE)
            return TranslateSavedESLFormID(formID);

        return (UInt32)g_SaveLoadResolveFormID(saveLoad, nullptr, (int)formID);
    }

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