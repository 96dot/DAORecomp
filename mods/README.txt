Mods for the Dragon Age: Origins recomp
=======================================

Each mod is a folder here that mirrors the game folder. Files in it replace or
add to the game's files, for example:

  mods/MyMod/packages/core/override/some_file.gda   (loose override files)
  mods/MyMod/packages/core/data/gui.erf             (replaces a whole archive)

The game itself loads loose files from packages/core/override, so that is the
usual place for small mods.

Optional: mods/MyMod/mod.toml describes the mod in the launcher:

  name = "My Mod"
  version = "1.0"
  author = "You"
  description = "What it does."

Enable, disable and order mods in the launcher's Mods tab. Mods lower in the
list win when two mods contain the same file. New folders start enabled.
