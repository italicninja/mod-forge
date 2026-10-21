/*
 * This file is part of the mod-forge module for AzerothCore, released under GNU GPL v2 license,
 * redistribute/modify under version 2 of the License, or (at your option) any later version.
 */

void AddForgeScripts();
void AddSC_forge_commandscript();
void AddSC_forge_currency();

// Called by the core's module loader (Add<module name with '-' as '_'>Scripts).
void Addmod_forgeScripts()
{
    AddForgeScripts();
    AddSC_forge_commandscript();
    AddSC_forge_currency();
}
