"""Copy files from src to dst only when their contents differ (keeps mtimes stable
so ninja rebuilds only what changed); delete dst files that src no longer has.

usage: sync_dir.py <src> <dst>
"""
import filecmp
import os
import shutil
import sys


def sync(src, dst):
    """Returns (changed, removed, unchanged) file counts."""
    os.makedirs(dst, exist_ok=True)
    changed = 0
    names = set(os.listdir(src))
    for name in sorted(names):
        s, d = os.path.join(src, name), os.path.join(dst, name)
        if not os.path.exists(d) or not filecmp.cmp(s, d, shallow=False):
            shutil.copyfile(s, d)
            changed += 1
    removed = 0
    for name in os.listdir(dst):
        if name not in names:
            os.remove(os.path.join(dst, name))
            removed += 1
    return changed, removed, len(names) - changed


def main():
    changed, removed, unchanged = sync(sys.argv[1], sys.argv[2])
    print(f'{changed} files updated, {removed} removed, {unchanged} unchanged')


if __name__ == '__main__':
    main()
