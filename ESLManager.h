#pragma once
#include <string>
#include <unordered_map>
#include <cstdint>

class ESLManager
{
public:
    static ESLManager& Get();

    bool     Initialize();

    void     ResetAssignments();

    uint16_t GetOrRegisterESLIndex(const std::string& pluginName);
    bool     IsESLPlugin(const std::string& pluginName) const;
    uint16_t GetESLIndex(const std::string& pluginName) const;
    bool     IsIndexValid(uint16_t index) const;

    const char* GetESLName(uint16_t eslIndex) const;

    void     RegisterFile(void* file, uint16_t eslIndex);

    void     ClearRuntimeState();
    uint16_t GetESLIndexForFile(void* file) const;

    bool     IsESLIndexActive(uint16_t eslIndex) const;

    void     ResetSaveRemap();
    void     SetSaveRemap(uint16_t savedIndex, uint16_t currentIndex);
    uint16_t RemapSavedIndex(uint16_t savedIndex) const;

    uint32_t Encode(uint16_t eslIndex, uint32_t localID) const;
    bool     IsEncoded(uint32_t formID) const;
    uint16_t DecodeIndex(uint32_t formID) const;
    uint32_t DecodeLocal(uint32_t formID) const;

    void     LoadPersistentMap();
    void     SavePersistentMap();

    static constexpr uint32_t kContainer = 0xFE000000;
    static constexpr uint16_t kMaxESL = 4096;
    static constexpr uint16_t kInvalid = 0xFFFF;

    static constexpr uint32_t kMinLocalID = 0x800;
    static constexpr uint32_t kMaxLocalID = 0xFFF;

    static bool IsValidLocalID(uint32_t localID)
    {
        return localID >= kMinLocalID && localID <= kMaxLocalID;
    }

private:
    ESLManager();

    std::unordered_map<std::string, uint16_t> m_nameToIndex;
    std::unordered_map<uint16_t, std::string> m_indexToName;
    uint16_t m_nextFree = 0;

    std::unordered_map<void*, uint16_t> m_fileToIndex;

    bool     m_activeIndex[kMaxESL];

    uint16_t m_saveRemap[kMaxESL];

    bool m_dirty = false;
};