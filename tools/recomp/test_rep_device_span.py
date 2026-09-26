import os
import unittest

from .disasm import Instruction
from .lifter import Lifter

_RUNTIME = os.path.join(os.path.dirname(__file__), "..", "..",
                        "templates", "runtime", "recomp_types.h")


def _lift(mnemonic):
    insn = Instruction(0, 2, mnemonic, "", "")
    insn.operands = []
    return "\n".join(Lifter().lift_instruction(insn))


class RepBlockCopyDeviceSpanTest(unittest.TestCase):
    """A block copy that reads device memory must not become memcpy: the
    fault lands in the C runtime, where the MMIO trap cannot emulate it.
    DirectSound's rep movsd from APU DSP memory (0xFE83040C) crashed so."""

    def test_every_block_fast_path_checks_both_spans(self):
        for m in ("rep movsb", "rep movsw", "rep movsd"):
            out = _lift(m)
            self.assertIn("RECOMP_RAM_SPAN(esi, _n) && RECOMP_RAM_SPAN(edi, _n)",
                          out, m)
            # The element loop is still there for everything else.
            self.assertIn("for (_i = 0;", out, m)

    def test_memset_fast_path_checks_its_span(self):
        out = _lift("rep stosb")
        self.assertIn("RECOMP_RAM_SPAN(edi, ecx)) { memset", out)
        self.assertIn("MEM8(edi + _i) = LO8(eax)", out)

    def test_runtime_defines_the_span_test_below_the_apertures(self):
        with open(_RUNTIME, encoding="utf-8") as fh:
            text = fh.read()
        self.assertIn("#define RECOMP_RAM_SPAN(va, n)", text)
        self.assertIn("<= 0xFD000000ull", text)


if __name__ == "__main__":
    unittest.main()
