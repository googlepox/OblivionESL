#pragma once
#include "obse/PluginAPI.h"

namespace ESLSerialization {

    void Register(const OBSEInterface* obse, PluginHandle handle);

    void SaveCallback(void* reserved);
    void LoadCallback(void* reserved);
}