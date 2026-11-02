"""Minimal MPQ support for building client patches (patch-4.MPQ).

Writes format v1 archives with every file stored uncompressed and unencrypted. The 3.3.5a
client accepts that, and it keeps the writer small. Reads v1/v2 headers far enough to check
whether an archive holds a file (to see which stock MPQ provides a DBC).
"""
import struct

_CRYPT = []


def _init_crypt():
    seed = 0x00100001
    table = [0] * 0x500
    for i in range(0x100):
        idx = i
        for _ in range(5):
            seed = (seed * 125 + 3) % 0x2AAAAB
            hi = (seed & 0xFFFF) << 16
            seed = (seed * 125 + 3) % 0x2AAAAB
            table[idx] = hi | (seed & 0xFFFF)
            idx += 0x100
    return table


_CRYPT = _init_crypt()

HASH_OFFSET, HASH_A, HASH_B, HASH_KEY = 0, 1, 2, 3


def hash_string(name, kind):
    s1, s2 = 0x7FED7FED, 0xEEEEEEEE
    for ch in name.upper().encode("ascii"):
        s1 = (_CRYPT[(kind << 8) + ch] ^ (s1 + s2)) & 0xFFFFFFFF
        s2 = (ch + s1 + s2 + (s2 << 5) + 3) & 0xFFFFFFFF
    return s1


def _crypt(data, key, decrypt):
    words = struct.unpack("<%dI" % (len(data) // 4), data)
    seed, out = 0xEEEEEEEE, []
    for w in words:
        seed = (seed + _CRYPT[0x400 + (key & 0xFF)]) & 0xFFFFFFFF
        r = (w ^ (key + seed)) & 0xFFFFFFFF
        plain = r if decrypt else w
        key = ((~key << 0x15) + 0x11111111 | key >> 0x0B) & 0xFFFFFFFF
        seed = (plain + seed + (seed << 5) + 3) & 0xFFFFFFFF
        out.append(r)
    return struct.pack("<%dI" % len(out), *out)


FILE_EXISTS = 0x80000000


def write_archive(path, files, sector_shift=3):
    """files: {"DBFilesClient\\Item.dbc": bytes}. A (listfile) is added."""
    files = dict(files)
    files["(listfile)"] = "\r\n".join(sorted(files)).encode("ascii") + b"\r\n"
    names = sorted(files)
    hash_size = 16
    while hash_size < len(names) * 2:
        hash_size *= 2

    body, blocks = bytearray(), []
    offset = 32
    for name in names:
        data = files[name]
        blocks.append((offset + len(body), len(data), len(data), FILE_EXISTS))
        body += data

    empty = (0xFFFFFFFF, 0xFFFFFFFF, 0xFFFF, 0xFFFF, 0xFFFFFFFF)
    table = [empty] * hash_size
    for bi, name in enumerate(names):
        i = hash_string(name, HASH_OFFSET) & (hash_size - 1)
        while table[i][4] != 0xFFFFFFFF:
            i = (i + 1) & (hash_size - 1)
        table[i] = (hash_string(name, HASH_A), hash_string(name, HASH_B), 0, 0, bi)

    hash_raw = b"".join(struct.pack("<IIHHI", *e) for e in table)
    block_raw = b"".join(struct.pack("<IIII", *b) for b in blocks)
    hash_enc = _crypt(hash_raw, hash_string("(hash table)", HASH_KEY), False)
    block_enc = _crypt(block_raw, hash_string("(block table)", HASH_KEY), False)

    hash_pos = 32 + len(body)
    block_pos = hash_pos + len(hash_enc)
    size = block_pos + len(block_enc)
    header = struct.pack("<4sIIHHIIII", b"MPQ\x1a", 32, size, 0, sector_shift,
                         hash_pos, block_pos, hash_size, len(blocks))
    with open(path, "wb") as f:
        f.write(header + body + hash_enc + block_enc)


def has_file(path, name):
    """True if the archive's hash table lists `name` (any locale)."""
    with open(path, "rb") as f:
        head = f.read(44)
        magic, hsize, _, ver, _, hpos, _, hcount, _ = struct.unpack("<4sIIHHIIII", head[:32])
        if magic != b"MPQ\x1a":
            raise ValueError("%s: not an MPQ (user-data header not supported)" % path)
        hi = struct.unpack("<HH", head[40:44])[0] if ver >= 1 and hsize >= 44 else 0
        f.seek(hpos | (hi << 32))
        raw = _crypt(f.read(hcount * 16), hash_string("(hash table)", HASH_KEY), True)
    a, b = hash_string(name, HASH_A), hash_string(name, HASH_B)
    i = start = hash_string(name, HASH_OFFSET) & (hcount - 1)
    while True:
        ea, eb, _, _, bi = struct.unpack_from("<IIHHI", raw, i * 16)
        if bi == 0xFFFFFFFF:
            return False
        if ea == a and eb == b and bi != 0xFFFFFFFE:
            return True
        i = (i + 1) & (hcount - 1)
        if i == start:
            return False


