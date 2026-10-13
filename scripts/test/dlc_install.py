"""dlc_install.py <packages folder> <userdata content folder>

Test install of DLC: extracts every STFS package in <packages folder> into
<content>/<title>/<content type>/<package>/, with the .name file (display name,
UTF-16BE, 256 bytes) the game expects, as the launcher's DLC install does. Packages
already there are skipped."""
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import stfs  # noqa: E402


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    src, content = Path(sys.argv[1]), Path(sys.argv[2])
    for name in sorted(os.listdir(src)):
        with stfs.Package(str(src / name)) as pkg:
            folder = content / f"{pkg.title_id:08X}" / f"{pkg.content_type:08X}" / name
            if folder.exists():
                print(f"{pkg.display_name}: already there")
                continue
            folder.parent.mkdir(parents=True, exist_ok=True)
            pkg.extract(str(folder))
        display = pkg.display_name.encode("utf-16-be")[:254]
        folder.with_name(name + ".name").write_bytes(display + bytes(256 - len(display)))
        print(f"{pkg.display_name}: done")
    return 0


if __name__ == "__main__":
    sys.exit(main())
