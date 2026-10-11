"""python install.py [--iso X] [--dest Y]: the window without arguments, the command line with them
(the disc image is needed only until its game files are extracted to Y/game, so not to update)."""
import argparse
import os
import sys

from . import deps, pipeline, toolchain
from .util import FROZEN, IS_WINDOWS, VERSION, InstallError, Reporter


def hide_own_console():
    """The executable is a console program (so its command line works in a terminal);
    when it was double-clicked, the console is its own: close it for the window."""
    if not (FROZEN and IS_WINDOWS):
        return
    import ctypes
    kernel32 = ctypes.windll.kernel32
    processes = (ctypes.c_uint32 * 2)()
    if kernel32.GetConsoleProcessList(processes, 2) == 1:
        kernel32.FreeConsole()


def ask_license():
    print(f"Building needs the Microsoft Visual C++ runtime headers/libraries and the Windows SDK,\n"
          f"which are downloaded from Microsoft. Their license: {deps.MSVC_LICENSE_URL}")
    if not sys.stdin.isatty():
        return False
    try:
        return input("Do you accept it? (yes/no) ").strip().lower() in ("y", "yes")
    except EOFError:
        return False


def main():
    prog = "DAORecomp-Installer.exe" if FROZEN else "install.py"
    parser = argparse.ArgumentParser(prog=prog, description=__doc__.replace("python install.py", prog))
    parser.add_argument("--iso", help="the Dragon Age: Origins (Xbox 360) disc image (not needed to update an "
                                      "install whose game files are extracted)")
    parser.add_argument("--dest", help="install folder (game files go into its game/ folder)")
    parser.add_argument("--title-update", help="Title Update 6's package (optional: needed by Awakening and the "
                                               "DLC campaigns; kept in the install's title_update/ folder)")
    parser.add_argument("--accept-msvc-license", action="store_true",
                        help=f"accept the license of the Microsoft build tools ({deps.MSVC_LICENSE_URL})")
    parser.add_argument("--llvm", default="auto",
                        help="'auto' (default), 'download' (the pinned LLVM), or an LLVM bin directory")
    parser.add_argument("--jobs", type=int, default=0, help="parallel compile jobs (default: all cores)")
    parser.add_argument("--prepare-only", action="store_true",
                        help="set up sources, toolchain and recompiler without a disc image (checks a system)")
    parser.add_argument("--shortcuts", action="store_true",
                        help="create Start menu and desktop shortcuts (Windows; the window has a checkbox for it)")
    parser.add_argument("--gui", action="store_true", help="open the window even with other arguments")
    parser.add_argument("--update", action="store_true",
                        help="with --gui: update the install in --dest, starting at once if nothing's missing "
                             "(the launcher's Update)")
    parser.add_argument("--version", action="version", version=VERSION)
    args = parser.parse_args()
    if args.update and not args.gui:
        parser.error("--update goes with --gui (on the command line, --dest alone updates)")

    if args.gui or not (args.iso or args.dest or args.prepare_only):
        try:
            from . import gui
        except ImportError:
            if args.gui or IS_WINDOWS:
                raise
            print("Tkinter is not installed (Debian/Ubuntu: python3-tk); use the command line:\n")
            parser.print_help()
            return 2
        if not IS_WINDOWS and not os.environ.get("DISPLAY") and not os.environ.get("WAYLAND_DISPLAY"):
            parser.print_help()
            return 2
        hide_own_console()
        gui.main(args.iso or "", args.dest or "", args.update)
        return 0

    if not args.prepare_only and not args.dest:
        parser.error("--dest is needed (and --iso, unless the game files are already extracted there)")
    # Accepted on an earlier run if the CRT/SDK are already set up.
    accept = args.accept_msvc_license or toolchain.winsysroot_ready() or ask_license()
    options = pipeline.Options(iso=args.iso, dest=args.dest, title_update=args.title_update,
                               accept_license=accept, llvm=args.llvm, jobs=args.jobs, shortcuts=args.shortcuts)
    reporter = Reporter()
    try:
        if args.prepare_only:
            pipeline.prepare_only(reporter, options)
        else:
            pipeline.run_all(reporter, options)
    except KeyboardInterrupt:
        print("\nStopped. Running the installer again resumes where it stopped.")
        return 1
    except InstallError as e:
        print(f"\nError: {e}\nRunning the installer again resumes where it stopped.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
