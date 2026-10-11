"""Gives a build's generated code names of its own, so two builds of the game
(the disc's and Title Update 6's) link into one executable: sub_XXXXXXXX (and
its __imp__ twin), the register save/restore helpers, the entry point and
PPCFuncMappings get the prefix. Imports (__imp__<export>) keep their names: both
builds share the runtime's kernel. Already prefixed names are left alone, so
running it twice changes nothing.

usage: prefix_symbols.py <generated dir> <prefix, e.g. tu6>
"""
import os
import re
import sys


def patterns(prefix):
    # Not \b at the start: the names also follow __imp__ (an underscore).
    sub = re.compile(rf'(?<![0-9A-Za-z])(?<!{prefix}_)sub_(?=[0-9A-F]{{8}}\b)')
    helper = re.compile(r'(?<![0-9A-Za-z])__(rest|save)(gprlr|fpr|vmx)_(\d+)\b')
    entry = re.compile(r'(?<![0-9A-Za-z])_xstart\b')
    mappings = re.compile(r'\bPPCFuncMappings\b')
    return [
        (sub, prefix + '_sub_'),
        (helper, lambda m: f'__{prefix}_{m.group(1)}{m.group(2)}_{m.group(3)}'),
        (entry, f'_{prefix}_xstart'),
        (mappings, f'PPCFuncMappings_{prefix}'),
    ]


def apply(folder, prefix):
    rules = patterns(prefix)
    for name in sorted(os.listdir(folder)):
        if not name.endswith(('.cpp', '.h')):
            continue
        path = os.path.join(folder, name)
        with open(path, encoding='utf-8', newline='') as f:
            text = f.read()
        changed = text
        for pattern, replacement in rules:
            changed = pattern.sub(replacement, changed)
        if changed != text:
            with open(path, 'w', encoding='utf-8', newline='') as f:
                f.write(changed)


def main():
    folder, prefix = sys.argv[1], sys.argv[2]
    apply(folder, prefix)
    print(f'prefixed {folder} with {prefix}')


if __name__ == '__main__':
    main()
