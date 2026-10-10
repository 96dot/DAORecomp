"""python install.py [--iso X --dest Y]: the window without arguments, the command line with them."""
import argparse
import os
import sys

from . import deps, pipeline, toolchain
from .util import IS_WINDOWS, InstallError, Reporter


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
    parser = argparse.ArgumentParser(prog="install.py", description=__doc__)
    parser.add_argument("--iso", help="the Dragon Age: Origins (Xbox 360) disc image")
    parser.add_argument("--dest", help="install folder (game files go into its game/ folder)")
    parser.add_argument("--accept-msvc-license", action="store_true",
                        help=f"accept the license of the Microsoft build tools ({deps.MSVC_LICENSE_URL})")
    parser.add_argument("--llvm", default="auto",
                        help="'auto' (default), 'download' (the pinned LLVM), or an LLVM bin directory")
    parser.add_argument("--jobs", type=int, default=0, help="parallel compile jobs (default: all cores)")
    parser.add_argument("--prepare-only", action="store_true",
                        help="set up sources, toolchain and recompiler without a disc image (checks a system)")
    parser.add_argument("--gui", action="store_true", help="open the window even with other arguments")
    args = parser.parse_args()

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
        gui.main(args.iso or "", args.dest or "")
        return 0

    if not args.prepare_only and not (args.iso and args.dest):
        parser.error("--iso and --dest go together")
    # Accepted on an earlier run if the CRT/SDK are already set up.
    accept = args.accept_msvc_license or toolchain.winsysroot_ready() or ask_license()
    options = pipeline.Options(iso=args.iso, dest=args.dest, accept_license=accept, llvm=args.llvm,
                               jobs=args.jobs)
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
