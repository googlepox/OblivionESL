#pragma once
#include "obse/GameData.h"

namespace ESLLoadPatch {

    // Installs the four patches to TESDataHandler_LoadFiles that keep ESL
    // plugins out of modsByID and load them from a separate list instead.
    // Call after ESLHooks::InstallHooks().
    bool InstallPatches();

    // The ESL currently being loaded, or nullptr.
    //
    // Once every ESL carries file index 0xFE, the FormID high byte no longer
    // identifies WHICH ESL a record came from -- so this replaces modIndex as
    // the key in SetFormID_Hook and ResolveFormID_Hook. It is valid only for
    // the duration of a LoadFile call issued by the ESL load pass.
    extern ModEntry::Data* g_currentLoadingESL;

    // Number of ESL plugins deferred out of the normal load order.
    UInt32 GetESLCount();

    // Drops the deferred ESL list. Call on teardown -- the engine frees its
    // ModEntry::Data objects when returning to the main menu, and every entry
    // here holds one.
    void ClearFileList();
}