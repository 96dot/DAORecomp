Mods for the Dragon Age: Origins recomp
=======================================

The easy way: drop a mod's archive (.zip, .7z, .rar), folder or files on the
launcher window. It makes the mod's folder here (named after the mod, with " (2)"
added if that's taken) and sorts the files into place:
  - packages/, core/ and override/ folders go where they belong;
  - anything else goes into packages/core/override/<mod name>/, or straight into
    override/ for a DAModder mod made for it (Type="CoreOverrideAddIn");
  - readmes and pictures stay at the top, and a DAModder Manifest.xml becomes
    mod.toml.
What you drop is only read. Drops with no files for the game, and archives
holding links, aren't added. If the mod has optional parts or files with the
same name, the launcher says so: keep only the ones you want (see its readme).
Archives with names in other alphabets can't be opened: extract them and drop
the folder.

By hand: each mod is a folder here that mirrors the game folder. Files in it
replace or add to the game's files, for example:

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

Mods must be made for the Xbox 360 version. PC mods with models, textures or
archives (.msh/.mmh and other GFF files marked PC, .dds, PC .erf, .dazip) can't
be read by this version and crash it, so the launcher marks them "(PC version)"
and doesn't load them.
