#include "ESLApi.h"
#include "ESLManager.h"
#include "obse/GameAPI.h"
#include <windows.h>

namespace ESLApi {

    // ── Why this exists ─────────────────────────────────────────────────────────
    //
    // DataHandler::GetModIndex scans modsByID and returns a UInt8. ESL plugins
    // are deliberately not in that array, so it returns 0xFF for every one of
    // them -- and even if it found them, a UInt8 cannot express the answer: an
    // ESL's identity is 0xFE plus a 12-bit index, not a single byte.
    //
    // So any OBSE plugin resolving forms by mod name is structurally broken
    // against ESLs. Sun's Height's ResolveForm is one example; it got a null
    // form back and retried every frame, which looked like a load hang.
    //
    // The fix is a wider query that returns the whole high portion of a FormID.
    // xOBSE calls this when its own lookup fails; if this plugin is not
    // installed the pointer is null and behaviour is unchanged.

    // Returns the high bits to OR with a local FormID:
    //   0xFE000000 | (eslIndex << 12)
    // or kInvalidBase when the name is not a loaded ESL.
    UInt32 __cdecl GetESLFormIDBase(const char* modName)
    {
        if (!modName || !modName[0])
            return kInvalidBase;

        ESLManager& manager = ESLManager::Get();

        UInt16 eslIndex = manager.GetESLIndex(modName);

        if (eslIndex == ESLManager::kInvalid)
            return kInvalidBase;

        // Registered in the persistent map but not loaded this session.
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
        // xOBSE exports the setter; resolving it dynamically means this plugin
        // still loads against a build that predates the export.
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