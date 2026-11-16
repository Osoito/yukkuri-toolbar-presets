"""Check whether HSP keeps variable names in memory in a usable layout.

If the names live in one contiguous, ordered blob then the shortcut block can be
anchored on the name itself, which is stronger than matching values because it
survives the player reconfiguring anything.
"""

import sys

import probe

NEEDLE = b"act_select_key_confing_1\x00"


def main():
    proc = probe.open_game(0)
    if proc is None:
        print("game is not running")
        return 1
    print("pid:", proc.pid)
    try:
        found = 0
        for base, size in proc.regions():
            buf = proc.read(base, size)
            if buf is None:
                continue
            pos = buf.find(NEEDLE)
            while pos != -1:
                found += 1
                addr = base + pos
                lo = max(0, pos - 160)
                print("\n=== occurrence %d at %#010x ===" % (found, addr))
                print("before:", buf[lo:pos])
                print("after :", buf[pos:pos + 320])
                if found >= 4:
                    return 0
                pos = buf.find(NEEDLE, pos + 1)
        print("\noccurrences found:", found)
    finally:
        proc.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
