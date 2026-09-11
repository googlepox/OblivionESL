#include "ESLApi.h"
#include "ESLManager.h"
#include "obse/GameAPI.h"

extern "C" {

    __declspec(dllexport) UInt32 __cdecl ESL_GetInterfaceVersion()
    {
        return ESLApi::kInterfaceVersion;
    }

    __declspec(dllexport) UInt32 __cdecl ESL_GetFormIDBase(const char* modName)
    {
        if (!modName || !modName[0])
            return ESLApi::kInvalidBase;

        ESLManager& manager = ESLManager::Get();

        UInt16 eslIndex = manager.GetESLIndex(modName);

        if (eslIndex == ESLManager::kInvalid)
            return ESLApi::kInvalidBase;

        if (!manager.IsESLIndexActive(eslIndex))
            return ESLApi::kInvalidBase;

        return 0xFE000000 | ((UInt32)eslIndex << 12);
    }

    __declspec(dllexport) const char* __cdecl ESL_GetNameByIndex(UInt16 eslIndex)
    {
        ESLManager& manager = ESLManager::Get();

        if (!manager.IsESLIndexActive(eslIndex))
            return nullptr;

        return manager.GetESLName(eslIndex);
    }

    __declspec(dllexport) UInt32 __cdecl ESL_GetLoadedCount()
    {
        UInt32 count = 0;

        ESLManager& manager = ESLManager::Get();

        for (UInt16 i = 0; i < ESLManager::kMaxESL; i++)
        {
            if (manager.IsESLIndexActive(i))
                count++;
        }

        return count;
    }

};

namespace ESLApi {

    void Register()
    {
        _MESSAGE("[ESL] API exports available (interface version %u).",
            kInterfaceVersion);
    }
}