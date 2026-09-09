#pragma once
#include "obse/PluginAPI.h"

namespace ESLApi {

    // Returned when a name is not a loaded ESL. Not a valid FormID base --
    // 0xFF000000 is the dynamic form space and IS valid, so it cannot be the
    // sentinel.
    static const UInt32 kInvalidBase = 0xFFFFFFFF;

    // Must match OBSE_ESLInterface in xOBSE's GameData.h.
    struct Interface
    {
        UInt32        version;
        UInt32(__cdecl* GetFormIDBase)(const char* modName);
        const char* (__cdecl* GetNameByIndex)(UInt16 eslIndex);
    };

    static const UInt32 kInterfaceVersion = 1;

    // High bits for a FormID belonging to the named ESL:
    //   0xFE000000 | (eslIndex << 12)
    UInt32 __cdecl GetESLFormIDBase(const char* modName);

    // Plugin name for an ESL index, so xOBSE can name a 0xFE FormID's source.
    const char* __cdecl GetESLNameByIndex(UInt16 eslIndex);

    // Hands the above to xOBSE so its name-based lookups can resolve ESLs.
    // Safe to call when the export is absent; it warns and does nothing.
    void Register();
}