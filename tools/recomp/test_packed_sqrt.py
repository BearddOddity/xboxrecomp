import unittest
from .disasm import Instruction, Operand
from .lifter import Lifter


class PackedSqrtTest(unittest.TestCase):
    """sqrtps/rsqrtps/rcpps must write all four lanes, not lane 0 only."""

    def test_register_source_uses_four_lane_helper(self):
        for mnemonic, helper in (('sqrtps', 'XMM_SQRT'), ('rsqrtps', 'XMM_RSQRT'), ('rcpps', 'XMM_RCP')):
            with self.subTest(mnemonic=mnemonic):
                instruction = Instruction(0, 3, mnemonic, 'xmm2, xmm2', '')
                instruction.operands = [Operand(type='reg', reg='xmm2'), Operand(type='reg', reg='xmm2')]
                result = '\n'.join(Lifter().lift_instruction(instruction))
                self.assertIn(f'{helper}(xmm2, xmm2)', result)
                self.assertNotIn('.f[0]', result)


if __name__ == '__main__':
    unittest.main()
