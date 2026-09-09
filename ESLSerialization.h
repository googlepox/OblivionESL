#pragma once
#include "obse/PluginAPI.h"

extern OBSEScriptInterface* g_scriptInterface;
extern OBSESerializationInterface* g_serialization;
extern OBSEStringVarInterface* g_stringVar;

namespace ESLSerialization {

    void Register(const OBSEInterface* obse, PluginHandle handle);

    void SaveCallback(void* reserved);
    void LoadCallback(void* reserved);
}