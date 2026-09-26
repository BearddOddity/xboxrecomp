import os
import shutil
import subprocess
import tempfile
import unittest

from .disasm import Instruction, Operand
from .lifter import Lifter


def _insn(mnemonic, op_str, operands):
    instruction = Instruction(0, 4, mnemonic, op_str, "")
    instruction.operands = operands
    return instruction


def _reg(name):
    return Operand(type="reg", reg=name)


def _imm(value):
    return Operand(type="imm", imm=value)


def _mem(base, disp=0, size=4):
    return Operand(type="mem", mem_base=base, mem_disp=disp, mem_size=size)


def _lift(mnemonic, op_str, operands):
    lifter = Lifter()
    lifter.needs_cf = True
    return lifter.lift_instruction(_insn(mnemonic, op_str, operands))


class MxcsrLifterTest(unittest.TestCase):
    """stmxcsr/ldmxcsr were no-ops, so the read-modify-write a title does at
    boot (set FTZ, set round-toward-zero) read garbage off the stack."""

    def test_store_and_load_go_through_guest_state(self):
        self.assertEqual(
            _lift("stmxcsr", "dword ptr [esp + 0x20]", [_mem("esp", 0x20)]),
            ["MEM32(esp + 0x20) = g_mxcsr; /* stmxcsr */"])
        self.assertEqual(
            _lift("ldmxcsr", "dword ptr [esp + 0x20]", [_mem("esp", 0x20)]),
            ["g_mxcsr = MEM32(esp + 0x20); /* ldmxcsr */"])


class RotateThroughCarryTest(unittest.TestCase):
    """The lifted C is compiled and run against a reference model, so the
    check is on behaviour, not on the spelling of the emitted text."""

    @classmethod
    def setUpClass(cls):
        cls.cc = shutil.which("cl") or shutil.which("gcc") or shutil.which("clang")

    def test_rotates_are_lifted_not_commented(self):
        for m in ("rcr", "rcl"):
            out = _lift(m, "eax, 1", [_reg("eax"), _imm(1)])
            self.assertEqual(len(out), 1)
            self.assertIn("_cf", out[0])
            self.assertTrue(out[0].startswith("{"), out)

    def _run(self, lines):
        if not self.cc:
            self.skipTest("no C compiler on PATH")
        src = ("#include <stdint.h>\n#include <stdio.h>\n"
               "int main(void) {\n uint32_t eax, ebx; int _cf;\n"
               + "\n".join(lines) + "\n return 0;\n}\n")
        d = tempfile.mkdtemp()
        try:
            c, exe = os.path.join(d, "t.c"), os.path.join(d, "t.exe")
            with open(c, "w") as fh:
                fh.write(src)
            if os.path.basename(self.cc).lower().startswith("cl"):
                cmd = [self.cc, "/nologo", c, "/Fe" + exe, "/Fo" + d + os.sep]
            else:
                cmd = [self.cc, c, "-o", exe]
            r = subprocess.run(cmd, capture_output=True, text=True, cwd=d)
            if r.returncode:
                self.skipTest("C compiler unusable here: " + (r.stdout + r.stderr)[-300:])
            return subprocess.run([exe], capture_output=True, text=True).stdout.split()
        finally:
            shutil.rmtree(d, ignore_errors=True)

    @staticmethod
    def _model(m, v, cf, n, w=32):
        n = (n & 31) % (w + 1) if w < 32 else n & 31
        for _ in range(n):
            if m == "rcr":
                v, cf = (v >> 1) | (cf << (w - 1)), v & 1
            else:
                v, cf = ((v << 1) | cf) & ((1 << w) - 1), (v >> (w - 1)) & 1
        return v, cf

    def test_results_match_the_hardware_model(self):
        cases = [(m, v, cf, n) for m in ("rcr", "rcl")
                 for v in (0, 1, 0x80000000, 0xDEADBEEF, 0xFFFFFFFF)
                 for cf in (0, 1) for n in (1, 3, 31, 32)]
        lines = []
        for m, v, cf, n in cases:
            body = _lift(m, "eax, %d" % n, [_reg("eax"), _imm(n)])[0]
            lines.append(' eax = %su; _cf = %d; %s printf("%%u %%d\\n", eax, _cf);'
                         % (v, cf, body))
        out = self._run(lines)
        for i, (m, v, cf, n) in enumerate(cases):
            want = self._model(m, v, cf, n)
            got = (int(out[2 * i]), int(out[2 * i + 1]))
            self.assertEqual(got, want, (m, hex(v), cf, n))

    def test_64bit_halving_idiom(self):
        # shr edx, 1 / rcr eax, 1 halves edx:eax; CF from shr feeds rcr.
        lines = [" ebx = 0x00000003u; eax = 0x00000000u; _cf = (int)(ebx & 1u);"
                 " ebx >>= 1;",
                 " " + _lift("rcr", "eax, 1", [_reg("eax"), _imm(1)])[0],
                 ' printf("%u %u\\n", ebx, eax);']
        self.assertEqual(self._run(lines), ["1", str(0x80000000)])


if __name__ == "__main__":
    unittest.main()
