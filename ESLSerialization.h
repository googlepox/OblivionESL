#pragma once
#include "obse/PluginAPI.h"

namespace ESLSerialization {

    // Records the active ESL list into the OBSE cosave and, on load, remaps
    // saved ESL indices onto current ones by plugin name.
    void Register(const OBSEInterface* obse, PluginHandle handle);

    void SaveCallback(void* reserved);
    void LoadCallback(void* reserved);
}