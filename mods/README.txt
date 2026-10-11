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

Enable, disable, order and delete mods in the launcher's Mods page (in the
game, Options > Graphics > Manage mods closes it and opens that page). Where two
mods change the same file, the one lower in the list usually wins (for files in
override/, which the game finds by name, the launcher warns of it). New folders
start enabled. Folders whose names start with "." aren't mods: the launcher
keeps its work there. Delete moves a mod's folder to the Recycle Bin.

Mods for the PC version
-----------------------
Most mods are made for the PC version, and not all of them can work on this one.
The launcher warns of what it finds in a mod that may not work (select the mod
to see the warnings), such as menus or sounds made for the PC, add-in areas and
campaigns, scripts newer than the game's, whole game tables, programs and very
large textures. It shows a notice about this the first time the Mods page opens.

PC models, textures and archives (.msh/.mmh and other GFF files marked PC,
.dds, PC .erf/.rim, .dazip) can't be read by this version and would crash it,
so the launcher marks such mods "(PC version)" and doesn't load them. Select one
and press Convert to turn its files into the Xbox 360 version's formats: GFF
files are rewritten in the 360's byte order and layouts, meshes' vertices and
.dds textures (to .xds) converted, archives and packages unpacked into
override/, and smaller copies made for distant models where the game has them.
Files that can't be converted are left out and listed. The mod's PC files are
moved to mods/.originals/<mod folder>/, keeping their paths, so you can undo it
by moving them back.
