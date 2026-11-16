"""Phase 0 probe: locate and poke the top shortcut launcher slots at runtime.

The nine top-launcher slots are the HSP variables act_select_key_confing_1..9.
Together with act_select_key_confing_a..z they form a run of 35 ints whose data
blocks sit 88 bytes apart, descending, in the game heap.  The base address moves
every run, so we locate the run by fingerprint: read the 35 values from key_save
on disk and search the process for a spot where all 35 match at that stride.

Usage:
    python probe.py                       show key_save and the live values
    python probe.py --set 1=500 4=1001    write action IDs into slots 1..9
    python probe.py --restore             put the key_save values back
"""

import argparse
import ctypes
import ctypes.wintypes as wt
import os
import struct
import sys
import time

import hspv

GAME_EXE = "ゆっくり八景.exe"
# Where the game is installed. Set YUKKURI_GAME_DIR when this checkout does not
# sit inside the game folder.
GAME_DIR = os.environ.get(
    "YUKKURI_GAME_DIR",
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
KEY_SAVE = os.path.join(GAME_DIR, "key_save")
BACKUP = os.path.join(os.path.dirname(os.path.abspath(__file__)), "slots_backup.txt")

SLOT_NAMES = ["act_select_key_confing_%s" % k
              for k in list("123456789") + list("abcdefghijklmnopqrstuvwxyz")]
STRIDE = 88            # gap between consecutive variable data blocks
SLOT_COUNT = len(SLOT_NAMES)

PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_READ = 0x0010
PROCESS_VM_WRITE = 0x0020
PROCESS_VM_OPERATION = 0x0008

MEM_COMMIT = 0x1000
MEM_IMAGE = 0x1000000
MEM_MAPPED = 0x40000
MEM_PRIVATE = 0x20000
PAGE_READWRITE = 0x04
PAGE_WRITECOPY = 0x08
PAGE_EXECUTE_READWRITE = 0x40
PAGE_EXECUTE_WRITECOPY = 0x80
PAGE_GUARD = 0x100

WRITABLE_PROTECTIONS = (PAGE_READWRITE | PAGE_WRITECOPY |
                        PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)
# HSP keeps variable data on its own heap (private), but the runtime also parks
# state in the image's data section, so scan both rather than guessing.
SCANNED_TYPES = (MEM_PRIVATE, MEM_IMAGE)

TH32CS_SNAPPROCESS = 0x00000002

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
advapi = ctypes.WinDLL("advapi32", use_last_error=True)

# Handles are pointer-sized; without these the return value is truncated to int
# on 64-bit Python and every subsequent call fails in confusing ways.
k32.OpenProcess.restype = wt.HANDLE
k32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
k32.CreateToolhelp32Snapshot.restype = wt.HANDLE
k32.CreateToolhelp32Snapshot.argtypes = [wt.DWORD, wt.DWORD]
k32.CloseHandle.argtypes = [wt.HANDLE]
k32.GetCurrentProcess.restype = wt.HANDLE
k32.VirtualQueryEx.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                               ctypes.c_size_t]
k32.VirtualQueryEx.restype = ctypes.c_size_t
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                                  ctypes.c_size_t, ctypes.c_void_p]
k32.WriteProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                                   ctypes.c_size_t, ctypes.c_void_p]


advapi.OpenProcessToken.argtypes = [wt.HANDLE, wt.DWORD, ctypes.c_void_p]
advapi.LookupPrivilegeValueW.argtypes = [wt.LPCWSTR, wt.LPCWSTR, ctypes.c_void_p]
advapi.AdjustTokenPrivileges.argtypes = [wt.HANDLE, wt.BOOL, ctypes.c_void_p,
                                         wt.DWORD, ctypes.c_void_p,
                                         ctypes.c_void_p]


class LUID(ctypes.Structure):
    _fields_ = [("LowPart", wt.DWORD), ("HighPart", ctypes.c_long)]


class LUID_AND_ATTRIBUTES(ctypes.Structure):
    _fields_ = [("Luid", LUID), ("Attributes", wt.DWORD)]


class TOKEN_PRIVILEGES(ctypes.Structure):
    _fields_ = [("PrivilegeCount", wt.DWORD),
                ("Privileges", LUID_AND_ATTRIBUTES * 1)]


def enable_debug_privilege():
    """Best-effort SeDebugPrivilege. Returns True if it was granted."""
    TOKEN_ADJUST_PRIVILEGES, TOKEN_QUERY = 0x0020, 0x0008
    SE_PRIVILEGE_ENABLED = 0x0002
    token = wt.HANDLE()
    if not advapi.OpenProcessToken(k32.GetCurrentProcess(),
                                   TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
                                   ctypes.byref(token)):
        return False
    try:
        luid = LUID()
        if not advapi.LookupPrivilegeValueW(None, "SeDebugPrivilege",
                                            ctypes.byref(luid)):
            return False
        tp = TOKEN_PRIVILEGES()
        tp.PrivilegeCount = 1
        tp.Privileges[0].Luid = luid
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED
        advapi.AdjustTokenPrivileges(token, False, ctypes.byref(tp), 0, None, None)
        return ctypes.get_last_error() == 0
    finally:
        k32.CloseHandle(token)


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD),
                ("cntUsage", wt.DWORD),
                ("th32ProcessID", wt.DWORD),
                ("th32DefaultHeapID", ctypes.POINTER(ctypes.c_ulong)),
                ("th32ModuleID", wt.DWORD),
                ("cntThreads", wt.DWORD),
                ("th32ParentProcessID", wt.DWORD),
                ("pcPriClassBase", ctypes.c_long),
                ("dwFlags", wt.DWORD),
                ("szExeFile", ctypes.c_wchar * 260)]


class MEMORY_BASIC_INFORMATION64(ctypes.Structure):
    _fields_ = [("BaseAddress", ctypes.c_ulonglong),
                ("AllocationBase", ctypes.c_ulonglong),
                ("AllocationProtect", wt.DWORD),
                ("__alignment1", wt.DWORD),
                ("RegionSize", ctypes.c_ulonglong),
                ("State", wt.DWORD),
                ("Protect", wt.DWORD),
                ("Type", wt.DWORD),
                ("__alignment2", wt.DWORD)]


def find_processes(name):
    """All pids whose image name matches, newest last.

    A crashed or leftover instance of the game can linger, so callers need to
    see every candidate rather than just the first one enumerated.
    """
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    if snap == -1:
        raise ctypes.WinError(ctypes.get_last_error())
    pids = []
    try:
        entry = PROCESSENTRY32W()
        entry.dwSize = ctypes.sizeof(entry)
        if not k32.Process32FirstW(snap, ctypes.byref(entry)):
            return pids
        while True:
            if entry.szExeFile.lower() == name.lower():
                pids.append(entry.th32ProcessID)
            if not k32.Process32NextW(snap, ctypes.byref(entry)):
                return pids
    finally:
        k32.CloseHandle(snap)


def find_process(name):
    pids = find_processes(name)
    return pids[0] if pids else None


class Process:
    def __init__(self, pid):
        access = (PROCESS_QUERY_INFORMATION | PROCESS_VM_READ |
                  PROCESS_VM_WRITE | PROCESS_VM_OPERATION)
        self.pid = pid
        self.handle = k32.OpenProcess(access, False, pid)
        if not self.handle:
            enable_debug_privilege()
            self.handle = k32.OpenProcess(access, False, pid)
        if not self.handle:
            raise ctypes.WinError(ctypes.get_last_error())

    def close(self):
        if self.handle:
            k32.CloseHandle(self.handle)
            self.handle = None

    def regions(self):
        """Yield (base, size) for committed, private, writable regions."""
        mbi = MEMORY_BASIC_INFORMATION64()
        addr = 0
        while addr < 0x7FFF0000:
            got = k32.VirtualQueryEx(self.handle, ctypes.c_void_p(addr),
                                     ctypes.byref(mbi), ctypes.sizeof(mbi))
            if not got:
                break
            base, size = int(mbi.BaseAddress), int(mbi.RegionSize)
            if size == 0:
                break
            writable = mbi.Protect & WRITABLE_PROTECTIONS
            if (mbi.State == MEM_COMMIT and mbi.Type in SCANNED_TYPES
                    and writable and not (mbi.Protect & PAGE_GUARD)):
                yield base, size
            addr = base + size

    def read(self, addr, size):
        buf = ctypes.create_string_buffer(size)
        got = ctypes.c_size_t(0)
        ok = k32.ReadProcessMemory(self.handle, ctypes.c_void_p(addr), buf,
                                   size, ctypes.byref(got))
        if not ok:
            return None
        return buf.raw[:got.value]

    def read_int(self, addr):
        raw = self.read(addr, 4)
        return None if raw is None or len(raw) < 4 else struct.unpack("<i", raw)[0]

    def write_int(self, addr, value):
        raw = struct.pack("<i", value)
        written = ctypes.c_size_t(0)
        ok = k32.WriteProcessMemory(self.handle, ctypes.c_void_p(addr), raw, 4,
                                    ctypes.byref(written))
        if not ok:
            raise ctypes.WinError(ctypes.get_last_error())
        return written.value == 4


def key_save_values(path=KEY_SAVE):
    """Return the 35 shortcut action IDs, ordered 1..9 then a..z."""
    table = hspv.by_name(path)
    missing = [n for n in SLOT_NAMES if n not in table]
    if missing:
        raise ValueError("key_save is missing %d expected vars, e.g. %s"
                         % (len(missing), missing[0]))
    return [table[n].ints[0] for n in SLOT_NAMES]


def fingerprint_quality(values):
    """How distinctive the fingerprint is: count of distinct non-zero values."""
    return len(set(v for v in values if v != 0))


# A scalar int PVal on this 32-bit build starts with these 28 constant bytes:
# flag=4, mode=1, len={1,1,0,0,0}, size=4.  Then pt, master=0, support=9.
PVAL_PREFIX = struct.pack("<HHiiiiii", 4, 1, 1, 1, 0, 0, 0, 4)
PVAL_SIZE = 48
LETTER_COUNT = 26          # act_select_key_confing_a .. _z


def scalar_int_pvals(proc):
    """Sorted (pval_addr, data_ptr) for every scalar int PVal in the process."""
    found = []
    for base, size in proc.regions():
        buf = proc.read(base, size)
        if buf is None:
            continue
        pos = buf.find(PVAL_PREFIX)
        while pos != -1:
            if pos % 4 == 0 and pos + 40 <= len(buf):
                pt, master, support = struct.unpack_from("<IIi", buf, pos + 28)
                if master == 0 and support == 9 and pt:
                    found.append((base + pos, pt))
            pos = buf.find(PVAL_PREFIX, pos + 1)
    # VirtualQuery can hand back overlapping views, so the same record may be
    # seen more than once; a duplicate would otherwise look like an ambiguous
    # match later on.
    return sorted(set(found))


def stride_runs(entries, minimum):
    """Index ranges of entries whose data pointers step down by exactly STRIDE."""
    runs = []
    i = 0
    while i < len(entries):
        j = i
        while j + 1 < len(entries) and entries[j + 1][1] == entries[j][1] - STRIDE:
            j += 1
        if j - i + 1 >= minimum:
            runs.append((i, j))
        i = j + 1
    return runs


def locate(proc, values, verbose=True):
    """Find the address of act_select_key_confing_1.

    Value fingerprints do not work here: the game only writes key_save when the
    letter-key config screen is used, so slots 1..9 drift away from the file as
    soon as you touch the top launcher.  Instead find the HSP variable table by
    its structure, then pin the block down with the 26 letter-key values, which
    do stay in sync with key_save.

    Returns a list of candidate addresses; normally exactly one.
    """
    letters = values[SLOT_COUNT - LETTER_COUNT:]
    entries = scalar_int_pvals(proc)
    if verbose:
        print("scalar int PVal records: %d" % len(entries))

    hits = []
    for start, end in stride_runs(entries, SLOT_COUNT):
        # The run's data blocks are contiguous, so pull them in one read and
        # index into the buffer rather than issuing thousands of tiny reads.
        count = end - start + 1
        top = entries[start][1]
        low = top - STRIDE * (count - 1)
        buf = proc.read(low, STRIDE * count)
        if buf is None:
            continue

        def value_at(k):
            """Value of the k-th entry counting down from the run's top."""
            return struct.unpack_from("<i", buf, (top - STRIDE * k) - low)[0]

        for off in range(0, count - SLOT_COUNT + 1):
            live = [value_at(off + k)
                    for k in range(SLOT_COUNT - LETTER_COUNT, SLOT_COUNT)]
            if live == letters:
                hits.append(entries[start + off][1])
    return sorted(set(hits))


def slot_addr(slot1_addr, n):
    """Address of slot n, 1-based."""
    return slot1_addr - STRIDE * (n - 1)


def read_slots(proc, slot1_addr, count=9):
    return [proc.read_int(slot_addr(slot1_addr, n)) for n in range(1, count + 1)]


def open_game(wait_seconds=0):
    """Return an open Process for the game, waiting for it to appear.

    Waiting also covers the case where the process exists but cannot be opened
    yet, which is what an integrity-level mismatch looks like.
    """
    deadline = time.time() + wait_seconds
    last_error = None
    while True:
        for pid in find_processes(GAME_EXE):
            try:
                return Process(pid)
            except OSError as exc:
                last_error = exc
        if time.time() >= deadline:
            if last_error is not None:
                raise last_error
            return None
        time.sleep(1.0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--set", nargs="*", metavar="N=ID",
                    help="write action ID into slot N (1..9)")
    ap.add_argument("--restore", action="store_true",
                    help="write the key_save values back into slots 1..9")
    ap.add_argument("--wait", type=int, default=0, metavar="SECONDS",
                    help="wait up to SECONDS for the game to be launched")
    args = ap.parse_args()

    values = key_save_values()
    quality = fingerprint_quality(values)
    print("key_save 1..9 :", values[:9])
    print("key_save a..z :", values[9:])
    print("fingerprint   : %d distinct non-zero values" % quality)
    if quality < 8:
        print("WARNING: weak fingerprint, configure more shortcuts in game first")

    proc = open_game(args.wait)
    if proc is None:
        print("\n%s is not running - start the game first." % GAME_EXE)
        return 1
    print("\npid           :", proc.pid)

    try:
        # The variables do not exist until HSP has run its initialisation, and
        # we may have caught the process the instant it was created, so keep
        # retrying rather than giving up on the first miss.
        deadline = time.time() + args.wait
        while True:
            hits = locate(proc, values)
            if hits or time.time() >= deadline:
                break
            time.sleep(2.0)

        if not hits:
            print("FAILED: fingerprint not found. The in-game shortcut config "
                  "probably changed since key_save was written, e.g. you loaded "
                  "a save. Open the shortcut settings and change one entry to "
                  "rewrite key_save, then retry.")
            return 2
        if len(hits) > 1:
            print("FAILED: %d candidates, fingerprint is ambiguous: %s"
                  % (len(hits), [hex(h) for h in hits]))
            return 3

        slot1 = hits[0]
        print("slot 1 addr   : %#010x" % slot1)
        print("slot 9 addr   : %#010x" % slot_addr(slot1, 9))
        live = read_slots(proc, slot1)
        print("live 1..9     :", live)
        print("matches disk  :", live == values[:9])

        if args.restore:
            if os.path.exists(BACKUP):
                with open(BACKUP) as fp:
                    restore_to = [int(x) for x in fp.read().split(",")]
                source = BACKUP
            else:
                restore_to, source = values[:9], "key_save"
            for n, v in enumerate(restore_to, start=1):
                proc.write_int(slot_addr(slot1, n), v)
            print("\nrestored slots 1..9 from %s: %s" % (source, restore_to))
        elif args.set:
            # Never clobber the player's live toolbar without a way back.
            if not os.path.exists(BACKUP):
                with open(BACKUP, "w") as fp:
                    fp.write(",".join(str(v) for v in live))
                print("backed up live slots to %s" % BACKUP)
            for item in args.set:
                n_str, _, v_str = item.partition("=")
                n, v = int(n_str), int(v_str)
                if not 1 <= n <= 9:
                    raise SystemExit("slot must be 1..9, got %d" % n)
                proc.write_int(slot_addr(slot1, n), v)
                print("slot %d <- %d" % (n, v))
            print("\nnow look at the game top launcher: did it change?")
            print("re-read 1..9  :", read_slots(proc, slot1))
    finally:
        proc.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
