"""Parser for HSP3 `savevar`-style files used by Yukkuri Hakkei.

Container layout (little endian):

    0x00  char[4] "hspv"
    0x04  u16     version (0x1000)
    0x06  u16     opt
    0x08  u32     count      - number of variable records
    0x0C  u32     hdrSize    - offset of the data area
    0x10  count records, 64 bytes each:
            +0x00 i32 nameOff   (relative to hdrSize)
            +0x04 i32 dataOff   (relative to hdrSize)
            +0x10 u16 flag      (2 = str, 3 = double, 4 = int)
            +0x12 u16 mode
            +0x14 i32 len[5]
            +0x28 i32 size      (bytes of data)
            +0x2C u32 pt        (runtime address the data lived at)
            +0x30 u32 master
            +0x34 i32 support
    hdrSize.. data area: for each record, the name then the value bytes.
"""

import struct

FLAG_LABEL = 1
FLAG_STR = 2
FLAG_DOUBLE = 3
FLAG_INT = 4

RECORD_SIZE = 64


class Var:
    __slots__ = ("index", "name", "flag", "mode", "dims", "size", "pt", "raw")

    def __init__(self, index, name, flag, mode, dims, size, pt, raw):
        self.index = index
        self.name = name
        self.flag = flag
        self.mode = mode
        self.dims = dims
        self.size = size
        self.pt = pt
        self.raw = raw

    @property
    def ints(self):
        return struct.unpack("<%di" % (len(self.raw) // 4), self.raw)

    @property
    def strings(self):
        return [s.decode("cp932", "replace") for s in self.raw.split(b"\x00")]

    def __repr__(self):
        return "<Var %d %s flag=%d dims=%s pt=%#010x>" % (
            self.index, self.name, self.flag, self.dims, self.pt)


def load(path):
    """Read a savevar file and return its list of Var records, in file order."""
    with open(path, "rb") as fp:
        data = fp.read()

    magic, _ver, _opt, count, hdr_size = struct.unpack_from("<4sHHII", data, 0)
    if magic != b"hspv":
        raise ValueError("%s is not an hspv file (magic %r)" % (path, magic))

    out = []
    for i in range(count):
        off = 16 + i * RECORD_SIZE
        name_off, data_off = struct.unpack_from("<2i", data, off)
        flag, mode = struct.unpack_from("<2H", data, off + 0x10)
        dims = struct.unpack_from("<5i", data, off + 0x14)[1:]
        size, pt = struct.unpack_from("<2I", data, off + 0x28)
        name = data[hdr_size + name_off:hdr_size + data_off].split(b"\x00")[0]
        raw = data[hdr_size + data_off:hdr_size + data_off + size]
        out.append(Var(i, name.decode("cp932", "replace"), flag, mode, dims,
                       size, pt, raw))
    return out


def by_name(path):
    return {v.name: v for v in load(path)}
