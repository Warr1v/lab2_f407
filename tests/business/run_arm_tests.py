"""Run isolated C test ELFs in Unicorn. No test doubles enter production ELF.
Install: python -m pip install --target build/test_python unicorn==2.1.4 pyelftools==0.32
Usage: python tests/business/run_arm_tests.py build/Debug/input_logic_tests.elf ...
"""
from pathlib import Path
import json
import sys
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "build/test_python"))
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS
from unicorn.arm_const import UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0, UC_ARM_REG_C1_C0_2

def run(path):
    uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
    for start, size in [(0x08000000,0x100000), (0x20000000,0x20000),
                        (0x10000000,0x10000), (0x30000000,0x1000),
                        (0x40000000,0x100000), (0xE0000000,0x100000)]:
        uc.mem_map(start, size)
    with Path(path).open("rb") as f:
        elf = ELFFile(f)
        for segment in elf.iter_segments():
            if segment['p_type'] == 'PT_LOAD' and segment['p_filesz']:
                uc.mem_write(segment['p_vaddr'], segment.data())
        syms = {s.name:s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
    uc.reg_write(UC_ARM_REG_C1_C0_2, 0xF00000)  # enable M4F instructions
    uc.reg_write(UC_ARM_REG_SP, 0x2001FFF0)
    uc.reg_write(UC_ARM_REG_LR, 0x30000001)
    uc.emu_start(syms['main'] | 1, 0x30000000, count=20000000)
    read = lambda name: int.from_bytes(uc.mem_read(syms[name],4),'little')
    result = dict(elf=str(path), checks=read('test_checks'), failed_line=read('test_failure'),
                  returned=uc.reg_read(UC_ARM_REG_PC)==0x30000000,
                  exit_code=uc.reg_read(UC_ARM_REG_R0))
    print(json.dumps(result))
    assert result['returned'] and result['exit_code']==0 and result['failed_line']==0, result
    return result

if __name__ == '__main__':
    results = [run(path) for path in sys.argv[1:]]
    mode=Path(sys.argv[1]).parent.name if len(sys.argv)>1 else 'none'
    out = ROOT / ('build/business_test_results_'+mode+'.json')
    out.write_text(json.dumps(results,indent=2), encoding='utf-8')
