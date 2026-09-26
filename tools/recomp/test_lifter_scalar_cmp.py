import unittest

from .disasm import Instruction, Operand
from .lifter import Lifter, _SCALAR_CMP_SS


def _insn(mnemonic, op_str, operands):
    instruction = Instruction(0, 4, mnemonic, op_str, "")
    instruction.operands = operands
    return instruction


def _xmm(name):
    return Operand(type="reg", reg=name)


def _mem(base=None, disp=0, size=4):
    return Operand(type="mem", mem_base=base, mem_disp=disp, mem_size=size)


class ScalarCompareLifterTest(unittest.TestCase):
    """cmpeqss/cmpltss used to be emitted as comments, so the mask that the
    following andps/andnps select reads was never written."""

    def test_eq_register_writes_lane_zero_mask(self):
        self.assertEqual(
            Lifter().lift_instruction(
                _insn("cmpeqss", "xmm0, xmm1", [_xmm("xmm0"), _xmm("xmm1")])),
            ["xmm0.u[0] = (xmm0.f[0] == xmm1.f[0]) ? 0xFFFFFFFFu : 0u;"
             " /* cmpeqss */"],
        )

    def test_lt_memory_operand_reads_a_float(self):
        self.assertEqual(
            Lifter().lift_instruction(
                _insn("cmpltss", "xmm2, dword ptr [eax + 8]",
                      [_xmm("xmm2"), _mem("eax", 8)])),
            ["xmm2.u[0] = (xmm2.f[0] < MEMF(eax + 8)) ? 0xFFFFFFFFu : 0u;"
             " /* cmpltss */"],
        )

    def test_every_predicate_is_lifted(self):
        for m in _SCALAR_CMP_SS:
            out = Lifter().lift_instruction(
                _insn(m, "xmm3, xmm4", [_xmm("xmm3"), _xmm("xmm4")]))
            self.assertEqual(len(out), 1, m)
            self.assertTrue(out[0].startswith("xmm3.u[0] = ("), out)

    def test_predicates_match_hardware_nan_rules(self):
        # Evaluate the emitted C tests with Python floats; the operators and
        # NaN behaviour are the same for ==, <, <=, != and not. eval() only
        # ever sees the lifter's own constant table, never external input.
        nan = float("nan")
        want = {  # (a, b) -> expected mask bit, per the CMPSS predicate table
            "cmpeqss":    {(1, 1): 1, (1, 2): 0, (nan, 1): 0},
            "cmpltss":    {(1, 2): 1, (2, 1): 0, (nan, 1): 0},
            "cmpless":    {(1, 1): 1, (2, 1): 0, (nan, 1): 0},
            "cmpunordss": {(1, 1): 0, (nan, 1): 1, (1, nan): 1},
            "cmpneqss":   {(1, 1): 0, (1, 2): 1, (nan, 1): 1},
            "cmpnltss":   {(1, 2): 0, (2, 1): 1, (nan, 1): 1},
            "cmpnless":   {(1, 1): 0, (2, 1): 1, (nan, 1): 1},
            "cmpordss":   {(1, 1): 1, (nan, 1): 0, (1, nan): 0},
        }
        for m, cases in want.items():
            expr = (_SCALAR_CMP_SS[m].format(a="a", b="b")
                    .replace("&&", "and").replace("||", "or")
                    .replace("!(", "not ("))
            for (a, b), bit in cases.items():
                self.assertEqual(int(bool(eval(expr))), bit, (m, a, b))


if __name__ == "__main__":
    unittest.main()
