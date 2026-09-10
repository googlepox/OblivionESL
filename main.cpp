#include "obse/PluginAPI.h"
#include "obse/CommandTable.h"
#include "obse/GameAPI.h"
#include "obse/GameData.h"
#include "obse_common/SafeWrite.h"

#include "ESLManager.h"
#include "ESLHooks.h"
#include "ESLLoadPatch.h"
#include "ESLSerialization.h"
#include "ESLApi.h"

#include <shlobj.h>
#include <string>

IDebugLog                   gLog("OblivionESL.log");

PluginHandle                g_pluginHandle = kPluginHandle_Invalid;
OBSEMessagingInterface* g_messaging = nullptr;

void MessageHandler(OBSEMessagingInterface::Message* msg)
{
    switch (msg->type)
    {
    case OBSEMessagingInterface::kMessage_LoadGame:
    case OBSEMessagingInterface::kMessage_ExitGame:
        ESLManager::Get().SavePersistentMap();
        break;

    case OBSEMessagingInterface::kMessage_ExitToMainMenu:
        ESLManager::Get().SavePersistentMap();
        ESLLoadPatch::ResetLoadedFlags();
        break;
    default:
        break;
    }
}

extern "C" {

    bool OBSEPlugin_Query(const OBSEInterface* obse, PluginInfo* info)
    {
        _MESSAGE("OblivionESL query");

        info->infoVersion = PluginInfo::kInfoVersion;
        info->name = "OblivionESL";
        info->version = 1;

        if (obse->isEditor)
        {
            _MESSAGE("Editor mode not supported, declining to load.");
            return false;
        }

        if (obse->obseVersion < OBSE_VERSION_INTEGER)
        {
            _ERROR("OBSE version too old (got %08X, expected at least %08X)",
                obse->obseVersion, OBSE_VERSION_INTEGER);
            return false;
        }

        if (obse->oblivionVersion != OBLIVION_VERSION)
        {
            _ERROR("Unsupported runtime version %08X (expected %08X)",
                obse->oblivionVersion, OBLIVION_VERSION);
            return false;
        }

        return true;
    }

    bool OBSEPlugin_Load(const OBSEInterface* obse)
    {
        _MESSAGE("OblivionESL load");

        g_pluginHandle = obse->GetPluginHandle();

        if (!ESLManager::Get().Initialize())
        {
            _ERROR("ESLManager failed to initialize, aborting.");
            return false;
        }

        if (!ESLHooks::InstallHooks())
        {
            _ERROR("Hook installation failed, aborting.");
            return false;
        }

        if (!ESLLoadPatch::InstallPatches())
        {
            _ERROR("Load order patches failed, aborting.");
            return false;
        }

        if (!ESLLoadPatch::InstallReloadHook())
        {
            _ERROR("Reload hook failed, aborting.");
            return false;
        }

        ESLApi::Register();

        g_messaging = (OBSEMessagingInterface*)obse->QueryInterface(kInterface_Messaging);

        if (g_messaging)
        {
            g_messaging->RegisterListener(g_pluginHandle, "OBSE", MessageHandler);
        }
        else
        {
            _WARNING("Messaging interface unavailable; ESL map will not be "
                "flushed automatically.");
        }

        _MESSAGE("OblivionESL loaded successfully.");
        return true;
    }

};