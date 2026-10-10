#!/usr/bin/env python3
"""Builds and installs Dragon Age: Origins (recompiled) from your own Xbox 360 disc image.

    python install.py                                  (opens the installer window)
    python install.py --iso <game.iso> --dest <folder> (command line)

Works on Windows and Linux (the result is a Windows program; on Linux it runs with
Wine/Proton). See README.md.
"""
import os
import sys

if sys.version_info < (3, 10):
    sys.exit("This installer needs Python 3.10 or newer.")

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from installer.__main__ import main  # noqa: E402

sys.exit(main())
