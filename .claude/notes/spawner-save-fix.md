# Spawner save fix (branch feature/spawner-save-fix)

A spawned Glitch Tarr breaks every autosave: Player.log shows `KeyNotFoundException: The given key 'SlimeDefinition.GlitchTarr' was not present in the dictionary` in AutoSaveDirector.SaveGameImpl. The save aborts before writing, so files are safe, but nothing saves while it exists.
- Only list identifiables the save system can persist (check against the same dictionary/lookup the save code uses, not a hardcoded blocklist).
- Gray out the Tarr entry when Tarrs are disabled in the game's settings.
