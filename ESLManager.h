#pragma once
#include <string>
#include <unordered_map>
#include <cstdint>

class ESLManager
{
public:
    static ESLManager& Get();

    bool     Initialize();

    // Plugin registration — single path for both preload and runtime
    uint16_t GetOrRegisterESLIndex(const std::string& pluginName);
    bool     IsESLPlugin(const std::string& pluginName) const;
    uint16_t GetESLIndex(const std::string& pluginName) const;
    bool     IsIndexValid(uint16_t index) const;

    // Runtime modIndex <-> ESL index mapping (flat array, fast lookup)
    void     RegisterRuntimeMapping(uint8_t modIndex, uint16_t eslIndex);
    bool     HasRuntimeMapping(uint8_t modIndex) const;
    uint16_t GetRuntimeESLIndex(uint8_t modIndex) const;

    // True if this ESL index belongs to a plugin loaded in THIS session.
    // Distinct from IsIndexValid(), which only says the index appears in the
    // persistent map -- that stays true for plugins the user has since removed.
    bool     IsESLIndexActive(uint16_t eslIndex) const;

    // FormID encoding/decoding
    uint32_t Encode(uint16_t eslIndex, uint32_t localID) const;
    bool     IsEncoded(uint32_t formID) const;
    uint16_t DecodeIndex(uint32_t formID) const;
    uint32_t DecodeLocal(uint32_t formID) const;

    // Persistence
    void     LoadPersistentMap();
    void     SavePersistentMap();

    static constexpr uint32_t kContainer = 0xFE000000;
    static constexpr uint16_t kMaxESL = 4096;
    static constexpr uint16_t kInvalid = 0xFFFF;

    // Usable local FormID range for a compacted plugin.
    //
    // The 0x800 floor is NOT arbitrary. Oblivion's TESForm_ResolveFormID skips
    // resolution entirely for IDs <= 0x7FF:
    //
    //     else if (!*a1 || *a1 > 0x7FF) { ...stamp load order byte... }
    //
    // That range is reserved for hardcoded engine forms, so a record numbered
    // below 0x800 never gets its high byte written and silently resolves as an
    // Oblivion.esm form. This is the same reason Skyrim ESLs use 0x800-0xFFF.
    //
    // Net effect: 2048 usable forms per ESL, not 4096.
    static constexpr uint32_t kMinLocalID = 0x800;
    static constexpr uint32_t kMaxLocalID = 0xFFF;

    static bool IsValidLocalID(uint32_t localID)
    {
        return localID >= kMinLocalID && localID <= kMaxLocalID;
    }

private:
    ESLManager();

    // Single source of truth for name <-> ESL index
    std::unordered_map<std::string, uint16_t> m_nameToIndex;
    std::unordered_map<uint16_t, std::string> m_indexToName;
    uint16_t m_nextFree = 0;

    // Fast flat arrays for runtime modIndex lookup — no map overhead
    uint16_t m_runtimeMapping[256];
    bool     m_hasMapping[256];

    // Reverse direction: ESL index -> loaded this session. Needed by
    // SaveLoad_ResolveFormID_Hook, which runs per FormID while a save loads,
    // so a linear scan of m_runtimeMapping would not do.
    bool     m_activeIndex[kMaxESL];

    bool m_dirty = false; // deferred save flag
};