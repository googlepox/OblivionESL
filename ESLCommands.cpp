#include <PluginAPI.h>
#include <GameAPI.h>

#include <ESLCommands.h>
#include <ESLInterface.h>
#include <ESLManager.h>
#include <ESLSerialization.h>

#include <StringVar.h>
#include <ModTable.h>

#define ExtractArgsEx(...) g_scriptInterface->ExtractArgsEx(__VA_ARGS__)
#define ExtractFormatStringArgs(...) g_scriptInterface->ExtractFormatStringArgs(__VA_ARGS__)


namespace ESLCommands {

    static UInt32 IdentifierFromBase(UInt32 base)
    {
        if (base == ESL::kInvalidBase)
            return 0xFF;

        return ((base >> 24) == 0xFE) ? (base >> 12) : (base >> 24);
    }

    static bool Cmd_ESLGetModIndex_Execute(COMMAND_ARGS)
    {
        char modName[512] = { 0 };

        *result = 0xFF;

        if (!ExtractArgs(PASS_EXTRACT_ARGS, modName))
            return true;

        UInt32 modIndex = IdentifierFromBase(ESL::GetFormIDBase(modName));

        *result = modIndex;

        if (IsConsoleMode())
        {
            if (modIndex == 0xFF)
                Console_Print("Mod Index: FF (not loaded)");
            else if (modIndex > 0xFF)
                Console_Print("Mod Index: %05X", modIndex);
            else
                Console_Print("Mod Index: %02X", modIndex);
        }

        return true;
    }

    static bool Cmd_ESLGetSourceModIndex_Execute(COMMAND_ARGS)
    {
        TESForm* form = NULL;

        *result = 0xFF;

        if (!ExtractArgsEx(paramInfo, arg1, opcodeOffsetPtr, scriptObj, eventList, &form))
            return true;

        if (!form)
            form = thisObj;

        if (!form)
            return true;

        if (form->IsCloned())
        {
            *result = 0xFF;
        }
        else if (ESL::IsESLFormID(form->refID))
        {
            *result = (UInt32)(form->refID >> 12);
        }
        else
        {
            *result = (UInt32)(form->refID >> 24);
        }

        if (IsConsoleMode())
        {
            UInt32 modIndex = (UInt32)*result;

            if (modIndex > 0xFF)
                Console_Print("Source Mod Index: %05X", modIndex);
            else
                Console_Print("Source Mod Index: %02X", modIndex);
        }

        return true;
    }

    static const char* ESLNameByOrdinal(UInt32 ordinal)
    {
        ESLManager& manager = ESLManager::Get();

        UInt32 seen = 0;

        for (UInt16 i = 0; i < ESLManager::kMaxESL; i++)
        {
            if (!manager.IsESLIndexActive(i))
                continue;

            if (seen == ordinal)
            {
                const char* name = manager.GetESLName(i);
                return name ? name : "";
            }

            seen++;
        }

        return "";
    }

    static bool Cmd_ESLGetNthModName_Execute(COMMAND_ARGS)
    {
        UInt32 modIdx = 0xFF;
        const char* modName = "";

        if (ExtractArgs(PASS_EXTRACT_ARGS, &modIdx))
        {
            if ((modIdx >> 12) == 0xFE)
            {
                const char* name = ESLManager::Get().GetESLName(
                    (UInt16)(modIdx & 0x0FFF));

                if (name)
                    modName = name;
            }
            else
            {
                UInt32 normalCount = (*g_dataHandler)->GetActiveModCount();

                if (modIdx < normalCount)
                {
                    const char* name = (*g_dataHandler)->GetNthModName(modIdx);

                    if (name)
                        modName = name;
                }
                else
                {
                    modName = ESLNameByOrdinal(modIdx - normalCount);
                }
            }
        }

        g_stringVar->Assign(PASS_COMMAND_ARGS, modName);

        return true;
    }

    static bool Cmd_ESLGetNumLoadedMods_Execute(COMMAND_ARGS)
    {
        *result = (*g_dataHandler)->GetActiveModCount() + ESL::LoadedCount();

        return true;
    }

    static bool Cmd_ESLIsModLoaded_Execute(COMMAND_ARGS)
    {
        char modName[512] = { 0 };

        *result = 0;

        if (!ExtractArgs(PASS_EXTRACT_ARGS, modName))
            return true;

        if (ModTable::Get().IsModLoaded(modName))
        {
            *result = 1.0;
        }
        else
        {
            ESLManager& manager = ESLManager::Get();

            UInt16 eslIndex = manager.GetESLIndex(modName);

            if (eslIndex != ESLManager::kInvalid &&
                manager.IsESLIndexActive(eslIndex))
            {
                *result = 1.0;
            }
        }

        if (IsConsoleMode())
            Console_Print(*result ? "Mod Loaded" : "Mod not loaded");

        return true;
    }

    static bool Cmd_ESLResolveModIndex_Execute(COMMAND_ARGS)
    {
        UInt32 storedModIndex = 0xFFFFFFFF;

        *result = -1.0;

        if (ExtractArgs(PASS_EXTRACT_ARGS, &storedModIndex))
        {
            if ((storedModIndex >> 12) == 0xFE)
            {
                ESLManager& manager = ESLManager::Get();

                UInt16 eslIndex = storedModIndex & 0x0FFF;

                if (eslIndex != ESLManager::kInvalid &&
                    manager.IsIndexValid(eslIndex) &&
                    manager.IsESLIndexActive(eslIndex))
                {
                    *result = (double)(0xFE000 | eslIndex);
                }
            }
            else if (storedModIndex < 0xFF)
            {
                UInt32 fixedRefID = 0;

                if (g_serialization->ResolveRefID(storedModIndex << 24, &fixedRefID))
                {
                    fixedRefID >>= 24;

                    if (fixedRefID != 0xFF)
                        *result = fixedRefID;
                }
            }
        }
        else {
            _MESSAGE("failed to extract");
        }

        if (IsConsoleMode())
            Console_Print("ResolveModIndex %d >> %.0f", storedModIndex, *result);

        return true;
    }

    void OverwriteOBSECommands()
    {
        UInt32 OBSECommandTablePatch = 0x004FCA68;
        CommandInfo* cmd = *(CommandInfo**)(OBSECommandTablePatch + 3);

        if (!cmd)
        {
            _ERROR("Overwrite OBSE Commands: command table not found at %08X",
                OBSECommandTablePatch);
            return;
        }

        _MESSAGE("Overwrite OBSE Commands: Command Table at %08X", cmd);

        struct Replacement
        {
            const char* name;
            bool (*execute)(COMMAND_ARGS);
        };

        static const Replacement kReplacements[] =
        {
            { "GetModIndex",        Cmd_ESLGetModIndex_Execute },
            { "GetSourceModIndex",  Cmd_ESLGetSourceModIndex_Execute },
            { "GetNthModName",      Cmd_ESLGetNthModName_Execute },
            { "GetNumLoadedMods",   Cmd_ESLGetNumLoadedMods_Execute },
            { "IsModLoaded",        Cmd_ESLIsModLoaded_Execute },
            { "ResolveModIndex",    Cmd_ESLResolveModIndex_Execute },
        };

        UInt32 replaced = 0;

        while (cmd->opcode)
        {
            for (const Replacement& r : kReplacements)
            {
                if (_stricmp(cmd->longName, r.name) == 0)
                {
                    _MESSAGE("Overwriting command '%s' w/ opcode %08X",
                        cmd->longName, cmd->opcode);

                    cmd->execute = r.execute;
                    replaced++;
                    break;
                }
            }

            cmd++;
        }

        _MESSAGE("Overwrite OBSE Commands: replaced %u of %u",
            replaced, (UInt32)(sizeof(kReplacements) / sizeof(kReplacements[0])));
    }
}