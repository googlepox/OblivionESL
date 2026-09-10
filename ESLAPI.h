#pragma once
#include "obse/PluginAPI.h"

namespace ESLApi {

    static const UInt32 kInvalidBase = 0xFFFFFFFF;

    struct Interface
    {
        UInt32        version;
        UInt32(__cdecl* GetFormIDBase)(const char* modName);
        const char* (__cdecl* GetNameByIndex)(UInt16 eslIndex);
    };

    static const UInt32 kInterfaceVersion = 1;

    UInt32 __cdecl GetESLFormIDBase(const char* modName);

    const char* __cdecl GetESLNameByIndex(UInt16 eslIndex);

    void Register();
}