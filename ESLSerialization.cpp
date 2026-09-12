#include "ESLSerialization.h"
#include "ESLManager.h"
#include "ESLLoadPatch.h"
#include "obse/GameAPI.h"
#include <vector>
#include <string>

namespace ESLSerialization {

    OBSESerializationInterface* g_serialization = nullptr;

    static const UInt32 kRecordType = 'ESLM';
    static const UInt32 kVersion = 1;

    void SaveCallback(void*)
    {
        if (!g_serialization)
            return;

        ESLManager& manager = ESLManager::Get();

        std::vector<std::pair<UInt16, std::string>> entries;

        for (UInt16 i = 0; i < ESLManager::kMaxESL; i++)
        {
            if (!manager.IsESLIndexActive(i))
                continue;

            const char* name = manager.GetESLName(i);

            if (name && name[0])
                entries.push_back({ i, name });
        }

        g_serialization->OpenRecord(kRecordType, kVersion);

        UInt32 count = (UInt32)entries.size();
        g_serialization->WriteRecordData(&count, sizeof(count));

        for (auto& entry : entries)
        {
            UInt16 index = entry.first;
            UInt16 nameLen = (UInt16)entry.second.size();

            g_serialization->WriteRecordData(&index, sizeof(index));
            g_serialization->WriteRecordData(&nameLen, sizeof(nameLen));
            g_serialization->WriteRecordData(entry.second.data(), nameLen);
        }

        _MESSAGE("[ESL] Wrote %u ESL entries to cosave", count);
    }

    void PreLoadCallback(void*)
    {
        return;
    }

    void LoadCallback(void*)
    {
        if (!g_serialization)
            return;

        ESLManager& manager = ESLManager::Get();

        manager.ResetSaveRemap();

        UInt32 type = 0, version = 0, length = 0;

        while (g_serialization->GetNextRecordInfo(&type, &version, &length))
        {
            if (type != kRecordType)
                continue;

            if (version != kVersion)
            {
                _WARNING("[ESL] Cosave record version %u unsupported", version);
                continue;
            }

            UInt32 count = 0;
            g_serialization->ReadRecordData(&count, sizeof(count));

            UInt32 matched = 0, missing = 0;

            for (UInt32 i = 0; i < count; i++)
            {
                UInt16 savedIndex = 0;
                UInt16 nameLen = 0;

                g_serialization->ReadRecordData(&savedIndex, sizeof(savedIndex));
                g_serialization->ReadRecordData(&nameLen, sizeof(nameLen));

                if (nameLen == 0 || nameLen > 512)
                {
                    _ERROR("[ESL] Corrupt cosave entry at %u", i);
                    break;
                }

                std::string name(nameLen, '\\0');
                g_serialization->ReadRecordData(name.data(), nameLen);

                UInt16 currentIndex = manager.GetESLIndex(name);

                if (currentIndex == ESLManager::kInvalid ||
                    !manager.IsESLIndexActive(currentIndex))
                {
                    manager.SetSaveRemap(savedIndex, ESLManager::kInvalid);
                    missing++;

                    _WARNING("[ESL] '%s' was in the save but is not loaded",
                        name.c_str());
                    continue;
                }

                manager.SetSaveRemap(savedIndex, currentIndex);
                matched++;

                if (savedIndex != currentIndex)
                {
                    _MESSAGE("[ESL] '%s' moved: saved ESL %03X -> current %03X",
                        name.c_str(), savedIndex, currentIndex);
                }
            }

            _MESSAGE("[ESL] Cosave: %u matched, %u missing", matched, missing);
        }
    }

    void NewGameCallback(void*)
    {
        ESLManager::Get().SavePersistentMap();
    }

    void Register(const OBSEInterface* obse, PluginHandle handle)
    {
        g_serialization =
            (OBSESerializationInterface*)obse->QueryInterface(kInterface_Serialization);

        if (!g_serialization)
        {
            _ERROR("[ESL] Serialization interface unavailable -- ESL content "
                "will not survive save and reload.");
            return;
        }

        g_serialization->SetSaveCallback(handle, SaveCallback);
        g_serialization->SetLoadCallback(handle, LoadCallback);
        g_serialization->SetNewGameCallback(handle, NewGameCallback);

        _MESSAGE("[ESL] Serialization callbacks registered.");
    }
}