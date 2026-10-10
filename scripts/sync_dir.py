"""Copy files from src to dst only when their contents differ (keeps mtimes stable
so ninja rebuilds only what changed); delete dst files that src no longer has.

usage: sync_dir.py <src> <dst>
"""
import filecmp
import os
import shutil
import sys


def main():
    src, dst = sys.argv[1], sys.argv[2]
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
    print(f'{changed} files updated, {removed} removed, {len(names) - changed} unchanged')


if __name__ == '__main__':
    main()
