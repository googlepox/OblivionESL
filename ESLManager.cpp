#include "ESLManager.h"
#include <obse/GameAPI.h>
#include <obse/GameData.h>
#include <fstream>
#include <cstring>
#include <windows.h>

namespace {

    const char* kMapDir = "Data\\OBSE\\Plugins\\OblivionESL";
    const char* kMapPath = "Data\\OBSE\\Plugins\\OblivionESL\\ESLMap.dat";

    bool EnsureMapDirectory()
    {
        static const char* kParts[] = {
            "Data",
            "Data\\OBSE",
            "Data\\OBSE\\Plugins",
            "Data\\OBSE\\Plugins\\OblivionESL"
        };

        for (const char* part : kParts)
        {
            if (CreateDirectoryA(part, nullptr))
                continue;

            DWORD err = GetLastError();

            if (err != ERROR_ALREADY_EXISTS)
            {
                _ERROR("[ESLManager] CreateDirectory('%s') failed with %u",
                    part, err);
                return false;
            }
        }

        return true;
    }
}

ESLManager::ESLManager()
{
    memset(m_activeIndex, 0, sizeof(m_activeIndex));

    ResetSaveRemap();
}

ESLManager& ESLManager::Get()
{
    static ESLManager instance;
    return instance;
}

bool ESLManager::Initialize()
{
    LoadPersistentMap();
    return true;
}

void ESLManager::ResetAssignments()
{
    m_nameToIndex.clear();
    m_indexToName.clear();
    m_nextFree = 0;
}

uint16_t ESLManager::GetOrRegisterESLIndex(const std::string& pluginName)
{
    auto it = m_nameToIndex.find(pluginName);
    if (it != m_nameToIndex.end())
        return it->second;

    if (m_nextFree >= kMaxESL)
    {
        _ERROR("[ESLManager] Exceeded %u ESL plugin limit! Cannot register: %s",
            kMaxESL, pluginName.c_str());
        return kInvalid;
    }

    uint16_t index = m_nextFree++;

    m_nameToIndex[pluginName] = index;
    m_indexToName[index] = pluginName;

    m_dirty = true;

    _MESSAGE("[ESLManager] Registered ESL: %s -> index %u",
        pluginName.c_str(), index);

    return index;
}

bool ESLManager::IsESLPlugin(const std::string& pluginName) const
{
    return m_nameToIndex.count(pluginName) != 0;
}

uint16_t ESLManager::GetESLIndex(const std::string& pluginName) const
{
    auto it = m_nameToIndex.find(pluginName);
    return (it != m_nameToIndex.end()) ? it->second : kInvalid;
}

bool ESLManager::IsIndexValid(uint16_t index) const
{
    return m_indexToName.count(index) != 0;
}

const char* ESLManager::GetESLName(uint16_t eslIndex) const
{
    auto it = m_indexToName.find(eslIndex);
    return (it != m_indexToName.end()) ? it->second.c_str() : nullptr;
}

void ESLManager::RegisterFile(void* file, uint16_t eslIndex)
{
    if (!file || eslIndex >= kMaxESL)
        return;

    m_fileToIndex[file] = eslIndex;
    m_activeIndex[eslIndex] = true;
}

void ESLManager::ClearRuntimeState()
{
    m_fileToIndex.clear();
    memset(m_activeIndex, 0, sizeof(m_activeIndex));

    _MESSAGE("[ESLManager] Runtime file state cleared.");
}

uint16_t ESLManager::GetESLIndexForFile(void* file) const
{
    if (!file)
        return kInvalid;

    auto it = m_fileToIndex.find(file);
    return (it != m_fileToIndex.end()) ? it->second : kInvalid;
}

bool ESLManager::IsESLIndexActive(uint16_t eslIndex) const
{
    return eslIndex < kMaxESL && m_activeIndex[eslIndex];
}

void ESLManager::ResetSaveRemap()
{
    for (uint16_t i = 0; i < kMaxESL; i++)
        m_saveRemap[i] = i;
}

void ESLManager::SetSaveRemap(uint16_t savedIndex, uint16_t currentIndex)
{
    if (savedIndex < kMaxESL)
        m_saveRemap[savedIndex] = currentIndex;
}

uint16_t ESLManager::RemapSavedIndex(uint16_t savedIndex) const
{
    return (savedIndex < kMaxESL) ? m_saveRemap[savedIndex] : kInvalid;
}

uint32_t ESLManager::Encode(uint16_t eslIndex, uint32_t localID) const
{
    return kContainer |
        ((uint32_t)(eslIndex & 0x0FFF) << 12) |
        (localID & 0x0FFF);
}

bool ESLManager::IsEncoded(uint32_t formID) const
{
    return (formID >> 24) == 0xFE;
}

uint16_t ESLManager::DecodeIndex(uint32_t formID) const
{
    return (formID >> 12) & 0x0FFF;
}

uint32_t ESLManager::DecodeLocal(uint32_t formID) const
{
    return formID & 0x0FFF;
}

void ESLManager::LoadPersistentMap()
{
    std::ifstream file(kMapPath, std::ios::binary);

    if (!file.is_open())
    {
        _MESSAGE("[ESLManager] No existing ESLMap.dat, starting fresh.");
        return;
    }

    uint32_t count = 0;
    file.read((char*)&count, sizeof(count));

    for (uint32_t i = 0; i < count; i++)
    {
        uint16_t index = 0;
        uint16_t len = 0;

        file.read((char*)&index, sizeof(index));
        file.read((char*)&len, sizeof(len));

        if (len == 0 || len > 512)
        {
            _ERROR("[ESLManager] Corrupt ESLMap.dat entry at index %u", i);
            break;
        }

        std::string name(len, '\0');
        file.read(name.data(), len);

        m_nameToIndex[name] = index;
        m_indexToName[index] = name;

        if (index >= m_nextFree)
            m_nextFree = index + 1;
    }

    _MESSAGE("[ESLManager] Loaded %u ESL mappings from disk.", count);
}

void ESLManager::SavePersistentMap()
{
    if (!m_dirty)
        return;

    if (!EnsureMapDirectory())
    {
        _ERROR("[ESLManager] Could not create '%s' (error %u)",
            kMapDir, GetLastError());
        return;
    }

    std::ofstream file(kMapPath, std::ios::binary | std::ios::trunc);

    if (!file.is_open())
    {
        _ERROR("[ESLManager] Failed to open '%s' for writing!", kMapPath);
        return;
    }

    uint32_t count = (uint32_t)m_nameToIndex.size();
    file.write((char*)&count, sizeof(count));

    for (auto& pair : m_nameToIndex)
    {
        uint16_t index = pair.second;
        uint16_t len = (uint16_t)pair.first.size();

        file.write((char*)&index, sizeof(index));
        file.write((char*)&len, sizeof(len));
        file.write(pair.first.data(), len);
    }

    m_dirty = false;

    _MESSAGE("[ESLManager] Saved %u ESL mappings to disk.", count);
}