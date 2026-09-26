import struct
import unittest

from .engine import DisasmEngine


class _Section:
    def __init__(self, va, size):
        self.virtual_addr = va
        self.virtual_size = size
        self.executable = True


class _Image:
    """Just enough of BinaryImage for resync_jump_tables."""

    def __init__(self, base, data):
        self.base_address = base
        self.image_size = len(data)
        self._data = data
        self._sec = _Section(base, len(data))

    def get_section_data(self, section):
        return self._data

    def get_section_at_va(self, va):
        return self._sec if self.base_address <= va < self.base_address + self.image_size else None

    def read_u32_at_va(self, va):
        o = va - self.base_address
        if 0 <= o and o + 4 <= len(self._data):
            return struct.unpack_from("<I", self._data, o)[0]
        return None


BASE = 0x1000


def _memmove_tail():
    """`neg ecx; jmp [ecx*4 + T]` with its arms below T, like MSVC memmove.

    0x00: neg ecx
    0x02: jmp dword ptr [ecx*4 + T]      (7 bytes)
    0x09: 3 bytes of padding
    0x0C: table, 4 entries below T and T itself (5 dwords)
    0x20: case code (nops) ... ret
    """
    code = bytearray(b"\x90" * 0x40)
    code[0:2] = b"\xf7\xd9"                      # neg ecx
    tbl = BASE + 0x0C + 4 * 4                   # base = last of 5 entries
    code[2:9] = b"\xff\x24\x8d" + struct.pack("<I", tbl)
    for i in range(5):                           # arms land in the case code
        struct.pack_into("<I", code, 0x0C + 4 * i, BASE + 0x20 + i)
    code[0x3F] = 0xC3                            # ret
    return bytes(code), tbl


class NegatedIndexJumpTableTest(unittest.TestCase):
    def test_table_below_its_base_is_skipped_as_data(self):
        data, tbl = _memmove_tail()
        eng = DisasmEngine(_Image(BASE, data))
        eng.decode_at(BASE)
        eng.decode_at(BASE + 9)   # the sweep decodes straight on through the table
        n = eng.resync_jump_tables()
        self.assertEqual(n, 1)
        # The table's bytes start four entries below the dispatch base, and
        # that is where a fall-through walk arrives, so that is the key.
        self.assertEqual(eng.jump_tables.get(BASE + 0x0C), tbl + 4)
        self.assertEqual(eng.jump_table_entries(tbl),
                         [BASE + 0x20 + i for i in range(5)])
        # Nothing hallucinated over the table survives.
        for a in range(BASE + 0x0C, tbl + 4):
            self.assertIsNone(eng.get_instruction(a))

    def test_plain_table_is_not_walked_backward(self):
        # Same bytes without the neg: the dispatch indexes forward only, and
        # one forward entry is too few to be a table -- nothing is skipped.
        data, tbl = _memmove_tail()
        data = b"\x90\x90" + data[2:]
        eng = DisasmEngine(_Image(BASE, data))
        eng.decode_at(BASE)
        eng.decode_at(BASE + 9)
        eng.resync_jump_tables()
        self.assertNotIn(BASE + 0x0C, eng.jump_tables)
        self.assertEqual(eng.jump_table_entries(tbl), [])


if __name__ == "__main__":
    unittest.main()
