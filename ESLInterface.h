#pragma once

#include <windows.h>
#include "obse/GameData.h"

namespace ESL {

    static const UInt32 kInvalidBase = 0xFFFFFFFF;

    namespace Detail {

        typedef UInt32(__cdecl* tGetInterfaceVersion)();
        typedef UInt32(__cdecl* tGetFormIDBase)(const char*);
        typedef const char* (__cdecl* tGetNameByIndex)(UInt16);
        typedef UInt32(__cdecl* tGetLoadedCount)();

        struct Api
        {
            bool                 resolved = false;
            bool                 present = false;
            tGetInterfaceVersion GetInterfaceVersion = nullptr;
            tGetFormIDBase       GetFormIDBase = nullptr;
            tGetNameByIndex      GetNameByIndex = nullptr;
            tGetLoadedCount      GetLoadedCount = nullptr;
        };

        inline Api& Get()
        {
            static Api api;

            if (api.resolved)
                return api;

            api.resolved = true;

            HMODULE dll = GetModuleHandleA("OblivionESL");

            if (!dll)
                return api;

            api.GetInterfaceVersion =
                (tGetInterfaceVersion)GetProcAddress(dll, "ESL_GetInterfaceVersion");
            api.GetFormIDBase =
                (tGetFormIDBase)GetProcAddress(dll, "ESL_GetFormIDBase");
            api.GetNameByIndex =
                (tGetNameByIndex)GetProcAddress(dll, "ESL_GetNameByIndex");
            api.GetLoadedCount =
                (tGetLoadedCount)GetProcAddress(dll, "ESL_GetLoadedCount");

            api.present = api.GetFormIDBase && api.GetNameByIndex;

            return api;
        }
    }

    inline bool Available()
    {
        return Detail::Get().present;
    }

    inline UInt32 InterfaceVersion()
    {
        Detail::Api& api = Detail::Get();

        return (api.present && api.GetInterfaceVersion)
            ? api.GetInterfaceVersion() : 0;
    }

    inline bool IsESLFormID(UInt32 formID)
    {
        return Available() && ((formID >> 24) == 0xFE);
    }

    inline UInt32 LocalMask(UInt32 base)
    {
        return ((base >> 24) == 0xFE) ? 0x00000FFFu : 0x00FFFFFFu;
    }

    inline UInt32 GetFormIDBase(const char* modName)
    {
        if (!modName || !modName[0])
            return kInvalidBase;

        Detail::Api& api = Detail::Get();

        if (api.present)
        {
            UInt32 base = api.GetFormIDBase(modName);

            if (base != kInvalidBase)
                return base;
        }

        UInt8 index = (*g_dataHandler)->GetModIndex(modName);

        return (index == 0xFF) ? kInvalidBase : ((UInt32)index << 24);
    }

    inline UInt32 ResolveFormID(const char* modName, UInt32 localID)
    {
        UInt32 base = GetFormIDBase(modName);

        if (base == kInvalidBase)
            return 0;

        return base | (localID & LocalMask(base));
    }

    inline const char* GetModNameForFormID(UInt32 formID)
    {
        UInt8 high = (formID >> 24) & 0xFF;

        if (high == 0xFF)
            return "";

        if (high == 0xFE)
        {
            Detail::Api& api = Detail::Get();

            if (api.present)
            {
                const char* name =
                    api.GetNameByIndex((UInt16)((formID >> 12) & 0x0FFF));

                return name ? name : "";
            }
        }

        const char* name = (*g_dataHandler)->GetNthModName(high);

        return name ? name : "";
    }

    inline UInt32 LoadedCount()
    {
        Detail::Api& api = Detail::Get();

        return (api.present && api.GetLoadedCount) ? api.GetLoadedCount() : 0;
    }
}