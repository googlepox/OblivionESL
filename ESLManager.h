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

    // Plugin name for an ESL index, or nullptr. Needed by script commands that
    // report which mod a form came from -- for a 0xFE form the load order byte
    // says only "some ESL", so the index has to be decoded back to a name.
    const char* GetESLName(uint16_t eslIndex) const;

    // File -> ESL index.
    //
    // This replaces the old modIndex-keyed mapping. Once ESL plugins are kept
    // out of modsByID they all carry file index 0xFE, so the load order byte no
    // longer identifies WHICH ESL anything belongs to -- the file pointer does.
    void     RegisterFile(void* file, uint16_t eslIndex);

    // Drops every ModEntry::Data* we hold, plus the active-index flags.
    //
    // Must be called on teardown (exit to main menu). The engine frees its
    // ModEntry::Data objects there, and m_fileToIndex matches purely on
    // address -- so stale entries both point at freed memory AND can be
    // wrongly matched by a later allocation that lands on the same address.
    //
    // Deliberately does NOT touch m_nameToIndex/m_indexToName/m_nextFree:
    // those are the persistent index assignments that keep existing saves
    // resolvable, and clearing them would renumber every ESL.
    void     ClearRuntimeState();
    uint16_t GetESLIndexForFile(void* file) const;

    // True if this ESL index belongs to a plugin loaded in THIS session.
    // Distinct from IsIndexValid(), which only says the index appears in the
    // persistent map -- that stays true for plugins the user has since removed.
    bool     IsESLIndexActive(uint16_t eslIndex) const;

    // Saved ESL index -> current ESL index, rebuilt from the cosave on load.
    //
    // A saved FormID carries the index its plugin had when the save was
    // written. If the user has since added or removed an ESL that index may
    // now mean a different plugin, so it has to be translated -- the same job
    // modRefIDTable does for normal plugins. ResetSaveRemap installs identity,
    // which is correct for a new game or a save with no ESL record.
    void     ResetSaveRemap();
    void     SetSaveRemap(uint16_t savedIndex, uint16_t currentIndex);
    uint16_t RemapSavedIndex(uint16_t savedIndex) const;

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

    // Live file -> ESL index. Rebuilt every session; the persistent map above
    // is what keeps the INDEX itself stable across load order changes.
    std::unordered_map<void*, uint16_t> m_fileToIndex;

    // Reverse direction: ESL index -> loaded this session. Needed by
    // SaveLoad_ResolveFormID_Hook, which runs per FormID while a save loads,
    // so a scan of m_fileToIndex would not do.
    bool     m_activeIndex[kMaxESL];

    uint16_t m_saveRemap[kMaxESL];

    bool m_dirty = false; // deferred save flag
};