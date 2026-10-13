"""fake_steam.py <folder> [--dump]

Makes a fake Steam folder for testing the launcher's Add to Steam without the real Steam:
steam.exe, an account with one other shortcut in its shortcuts.vdf, and an account with
none. Start the launcher with DAO_STEAM_ROOT=<folder>, add the game, then --dump prints
each account's shortcuts.vdf (the other shortcut must survive) and grid artwork."""
import os
import struct
import sys

root = sys.argv[1]


def kv_string(name, value):
    return b"\x01" + name.encode() + b"\0" + value.encode() + b"\0"


def kv_int(name, value):
    return b"\x02" + name.encode() + b"\0" + struct.pack("<I", value)


def parse(data, pos, depth):
    out = []
    while pos < len(data):
        t = data[pos]
        pos += 1
        if t == 8:
            return out, pos
        end = data.index(b"\0", pos)
        name = data[pos:end].decode()
        pos = end + 1
        if t == 0:
            sub, pos = parse(data, pos, depth + 1)
            out.append((name, sub))
        elif t == 1:
            end = data.index(b"\0", pos)
            out.append((name, data[pos:end].decode()))
            pos = end + 1
        elif t in (2, 3, 4, 6):
            out.append((name, struct.unpack_from("<I", data, pos)[0]))
            pos += 4
        elif t in (7, 10):
            out.append((name, struct.unpack_from("<Q", data, pos)[0]))
            pos += 8
        else:
            raise ValueError(t)
    raise ValueError("no end")


def dump(nodes, indent=0):
    for name, value in nodes:
        if isinstance(value, list):
            print(" " * indent + name)
            dump(value, indent + 2)
        else:
            print(" " * indent + f"{name} = {value!r}")


if "--dump" in sys.argv:
    for account in sorted(os.listdir(os.path.join(root, "userdata"))):
        f = os.path.join(root, "userdata", account, "config", "shortcuts.vdf")
        data = open(f, "rb").read()
        nodes, pos = parse(data, 0, 0)
        print(f"== {account}: {len(data)} bytes, parsed to {pos}, trailing {len(data) - pos}")
        dump(nodes)
        grid = os.path.join(root, "userdata", account, "config", "grid")
        print("grid:", sorted(os.listdir(grid)) if os.path.isdir(grid) else None)
    sys.exit()

os.makedirs(os.path.join(root, "userdata", "11111111", "config"), exist_ok=True)
os.makedirs(os.path.join(root, "userdata", "22222222"), exist_ok=True)
open(os.path.join(root, "steam.exe"), "wb").close()
entry = (kv_int("appid", 0x81234567) + kv_string("AppName", "Other Game") + kv_string("Exe", '"C:\\Other\\other.exe"')
         + kv_string("StartDir", '"C:\\Other\\"') + b"\x00tags\0" + kv_string("0", "favorite") + b"\x08" + b"\x08")
vdf = b"\x00shortcuts\0" + b"\x000\0" + entry + b"\x08" + b"\x08"
open(os.path.join(root, "userdata", "11111111", "config", "shortcuts.vdf"), "wb").write(vdf)
print("made", root)
