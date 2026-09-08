#include "obse/PluginAPI.h"
#include "obse/CommandTable.h"
#include "obse/GameAPI.h"
#include "obse/GameData.h"
#include "obse_common/SafeWrite.h"

#include "ESLManager.h"
#include "ESLHooks.h"
#include "ESLLoadPatch.h"
#include "ESLSerialization.h"

#include <shlobj.h>
#include <string>

IDebugLog                   gLog("OblivionESL.log");

PluginHandle                g_pluginHandle = kPluginHandle_Invalid;
OBSEMessagingInterface* g_messaging = nullptr;

// ─────────────────────────────────────────────────────────────────────────────
// Message handler
//
// There is no OBSE message that means "the data handler has finished loading
// all plugins", so the ESL map is flushed at the points where that is
// guaranteed to already have happened: entering a game, and shutdown.
// SavePersistentMap() is guarded by ESLManager's m_dirty flag, so calling it
// repeatedly costs nothing when there is nothing new to write.
// ─────────────────────────────────────────────────────────────────────────────

void MessageHandler(OBSEMessagingInterface::Message* msg)
{
    switch (msg->type)
    {
    case OBSEMessagingInterface::kMessage_LoadGame:
    case OBSEMessagingInterface::kMessage_ExitGame:
    case OBSEMessagingInterface::kMessage_ExitToMainMenu:
        ESLManager::Get().SavePersistentMap();
        ESLManager::Get().ClearRuntimeState();   // m_fileToIndex, m_activeIndex
        ESLLoadPatch::ClearFileList();           // s_eslFiles
        ESLHooks::ClearESLCache();
        break;

    default:
        break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────

extern "C" {

    bool OBSEPlugin_Query(const OBSEInterface* obse, PluginInfo* info)
    {
        _MESSAGE("OblivionESL query");

        info->infoVersion = PluginInfo::kInfoVersion;
        info->name = "OblivionESL";
        info->version = 1;

        if (obse->isEditor)
        {
            // The CS has an entirely different FormID pipeline and none of these
            // hook addresses apply to it. Refuse rather than crash.
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
            // Every hook address in ESLHooks.cpp is hardcoded for one exe build.
            // Loading against a different runtime would detour arbitrary code.
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

        // 1. Bring up the manager first. Initialize() loads the persistent
        //    name -> ESL index map from disk, and the hooks below start consulting
        //    it as soon as the first plugin loads.
        if (!ESLManager::Get().Initialize())
        {
            _ERROR("ESLManager failed to initialize, aborting.");
            return false;
        }

        // 2. Install the detours. This must happen before the data handler starts
        //    loading plugins -- LoadFile_Hook is what registers each ESL and
        //    builds its runtime modIndex mapping.
        if (!ESLHooks::InstallHooks())
        {
            _ERROR("Hook installation failed, aborting.");
            return false;
        }

        // 3. Patch the load order handling so ESL plugins never consume one of
        //    the 255 slots. Must come after the hooks: AppendFile calls
        //    ESLHooks::IsESLFile.
        if (!ESLLoadPatch::InstallPatches())
        {
            _ERROR("Load order patches failed, aborting.");
            return false;
        }

        // 4. Cosave serialization. The vanilla save records its plugin list from
        //    modsByID, which ESLs are not in, so without this every ESL-sourced
        //    form looks orphaned on load.
        ESLSerialization::Register(obse, g_pluginHandle);

        // 5. Messaging is optional -- it is only used to flush the map to disk.
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