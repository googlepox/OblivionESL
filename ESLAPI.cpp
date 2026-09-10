#include "ESLApi.h"
#include "ESLManager.h"
#include "obse/GameAPI.h"
#include <windows.h>

namespace ESLApi {

    UInt32 __cdecl GetESLFormIDBase(const char* modName)
    {
        if (!modName || !modName[0])
            return kInvalidBase;

        ESLManager& manager = ESLManager::Get();

        UInt16 eslIndex = manager.GetESLIndex(modName);

        if (eslIndex == ESLManager::kInvalid)
            return kInvalidBase;

        if (!manager.IsESLIndexActive(eslIndex))
            return kInvalidBase;

        return 0xFE000000 | ((UInt32)eslIndex << 12);
    }

    const char* __cdecl GetESLNameByIndex(UInt16 eslIndex)
    {
        ESLManager& manager = ESLManager::Get();

        if (!manager.IsESLIndexActive(eslIndex))
            return nullptr;

        return manager.GetESLName(eslIndex);
    }

    static const Interface s_interface =
    {
        kInterfaceVersion,
        GetESLFormIDBase,
        GetESLNameByIndex
    };

    void Register()
    {
        HMODULE obse = GetModuleHandleA("obse_1_2_416");

        if (!obse)
        {
            _WARNING("[ESL] Could not find obse_1_2_416 module; "
                "name-based form lookup will not resolve ESLs.");
            return;
        }

        typedef void(__cdecl* tSetInterface)(const Interface*);

        tSetInterface setInterface =
            (tSetInterface)GetProcAddress(obse, "OBSE_SetESLInterface");

        if (!setInterface)
        {
            _WARNING("[ESL] xOBSE has no OBSE_SetESLInterface export; "
                "name-based form lookup will not resolve ESLs. "
                "Rebuild xOBSE with the ESL support hook.");
            return;
        }

        setInterface(&s_interface);

        _MESSAGE("[ESL] Registered ESL interface with xOBSE (version %u).",
            kInterfaceVersion);
    }
}