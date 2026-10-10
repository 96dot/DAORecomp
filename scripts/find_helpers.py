"""Locate the PPC register save/restore helpers XenonRecomp needs (by byte pattern)."""
import sys
img = open(sys.argv[1], 'rb').read()
BASE = 0x82000000
pats = {
    'restgprlr_14_address': 'e9c1ff68',
    'savegprlr_14_address': 'f9c1ff68',
    'restfpr_14_address':   'c9ccff70',
    'savefpr_14_address':   'd9ccff70',
    'restvmx_14_address':   '3960fee07dcb60ce',
    'savevmx_14_address':   '3960fee07dcb61ce',
    'restvmx_64_address':   '3960fc00100b60cb',
    'savevmx_64_address':   '3960fc00100b61cb',
}
for k, p in pats.items():
    b = bytes.fromhex(p)
    hits, i = [], img.find(b)
    while i != -1:
        if i % 4 == 0:
            hits.append(BASE + i)
        i = img.find(b, i + 4)
    print(f"{k} = {', '.join(hex(h).upper().replace('X','x') for h in hits[:6])}  ({len(hits)} hits)")
