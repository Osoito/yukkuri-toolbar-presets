"""Structural locator: find the HSP PVal table instead of matching values.

Every scalar int variable in HSP3 has a PVal record that looks like this on a
32-bit build:

    +0x00 u16 flag    = 4   (int)
    +0x02 u16 mode    = 1   (allocated)
    +0x04 i32 len[0]  = 1
    +0x08 i32 len[1]  = 1
    +0x0C i32 len[2]  = 0
    +0x10 i32 len[3]  = 0
    +0x14 i32 len[4]  = 0
    +0x18 i32 size    = 4
    +0x1C ptr pt      = address of the 4 bytes holding the value
    +0x20 ptr master  = 0
    +0x24 i32 support = 9

Bytes 0x00..0x1B are constant for every such variable, which makes a 28-byte
needle.  The 35 shortcut variables sit next to each other in the table and their
`pt` values descend by 88, so once the table is found the block is unmistakable
without knowing a single action ID.
"""

import struct
import sys

import probe

STRIDE = 88
BLOCK = 35

PVAL_PREFIX = struct.pack("<HHiiiiii", 4, 1, 1, 1, 0, 0, 0, 4)


def scalar_int_pvals(proc):
    """Yield (pval_addr, data_ptr) for every scalar int PVal in the process."""
    for base, size in proc.regions():
        buf = proc.read(base, size)
        if buf is None:
            continue
        pos = buf.find(PVAL_PREFIX)
        while pos != -1:
            if pos % 4 == 0 and pos + 40 <= len(buf):
                pt, master, support = struct.unpack_from("<IIi", buf, pos + 28)
                if master == 0 and support == 9 and pt:
                    yield base + pos, pt
            pos = buf.find(PVAL_PREFIX, pos + 1)


def main():
    proc = probe.open_game(0)
    if proc is None:
        print("game is not running")
        return 1
    print("pid:", proc.pid)
    try:
        entries = sorted(scalar_int_pvals(proc))
        print("scalar int PVal records found:", len(entries))
        if not entries:
            return 2

        # How are the records themselves spaced?  A single table shows up as one
        # dominant stride.
        gaps = {}
        for (a, _), (b, _) in zip(entries, entries[1:]):
            gaps[b - a] = gaps.get(b - a, 0) + 1
        top = sorted(gaps.items(), key=lambda kv: -kv[1])[:5]
        print("most common gaps between records:", top)

        # Find runs of BLOCK consecutive records whose data pointers step down
        # by exactly STRIDE.  That is the shortcut variable block.
        runs = []
        i = 0
        while i < len(entries):
            j = i
            while (j + 1 < len(entries)
                   and entries[j + 1][1] == entries[j][1] - STRIDE):
                j += 1
            if j - i + 1 >= BLOCK:
                runs.append((i, j))
            i = j + 1
        print("runs of >=%d records with -%d pt stride: %d"
              % (BLOCK, STRIDE, len(runs)))

        for start, end in runs:
            n = end - start + 1
            first_pt = entries[start][1]
            print("\nrun of %d, first pt=%#010x, pval=%#010x"
                  % (n, first_pt, entries[start][0]))
            vals = [proc.read_int(first_pt - STRIDE * k) for k in range(n)]
            print("  values:", vals)
    finally:
        proc.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
