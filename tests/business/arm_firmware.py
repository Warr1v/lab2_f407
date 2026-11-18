"""Small harness for actual firmware entry points; hardware is modeled explicitly."""
from pathlib import Path
import sys
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'build/test_python'))
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_CODE, UC_HOOK_MEM_WRITE
from unicorn.arm_const import UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_C1_C0_2

class Firmware:
    def __init__(self, path=ROOT/'build/Debug/lab2_f407.elf'):
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        for start,size in [(0x08000000,0x100000),(0x20000000,0x20000),
                           (0x40000000,0x100000),(0xE0000000,0x100000),(0x30000000,0x1000)]:
            self.uc.mem_map(start,size)
        with Path(path).open('rb') as f:
            elf=ELFFile(f)
            for segment in elf.iter_segments():
                if segment['p_type']=='PT_LOAD' and segment['p_filesz']:
                    self.uc.mem_write(segment['p_vaddr'],segment.data())
            self.syms={s.name:s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
            self.layouts={}
            for cu in elf.get_dwarf_info().iter_CUs():
                for die in cu.iter_DIEs():
                    if die.tag!='DW_TAG_typedef' or 'DW_AT_name' not in die.attributes: continue
                    name=die.attributes['DW_AT_name'].value.decode()
                    if name not in ('InputSnapshot','SweepSnapshot','InputEdges'): continue
                    struct_die=die.get_DIE_from_attribute('DW_AT_type')
                    fields={}
                    for field in struct_die.iter_children():
                        if 'DW_AT_data_member_location' not in field.attributes: continue
                        typ=field.get_DIE_from_attribute('DW_AT_type')
                        while 'DW_AT_byte_size' not in typ.attributes:
                            typ=typ.get_DIE_from_attribute('DW_AT_type')
                        fields[field.attributes['DW_AT_name'].value.decode()]=(
                            field.attributes['DW_AT_data_member_location'].value,
                            typ.attributes['DW_AT_byte_size'].value)
                    self.layouts[name]=fields
        self.uc.reg_write(UC_ARM_REG_C1_C0_2,0xF00000)
        self.now=0
        self.hook('HAL_GetTick',lambda:self.now)
    def read(self,addr): return int.from_bytes(self.uc.mem_read(addr,4),'little')
    def write(self,addr,value): self.uc.mem_write(addr,(value&0xFFFFFFFF).to_bytes(4,'little'))
    def structure(self,name,addr):
        return {field:int.from_bytes(self.uc.mem_read(addr+offset,size),'little')
                for field,(offset,size) in self.layouts[name].items()}
    def hook(self,name,callback):
        def intercept(uc,addr,size,data):
            value=callback()
            if value is not None: uc.reg_write(UC_ARM_REG_R0,value)
            uc.reg_write(UC_ARM_REG_PC,uc.reg_read(UC_ARM_REG_LR))
        address=self.syms[name]&~1
        self.uc.hook_add(UC_HOOK_CODE,intercept,begin=address,end=address)
    def call(self,name,*args):
        self.uc.reg_write(UC_ARM_REG_SP,0x2001FFF0)
        self.uc.reg_write(UC_ARM_REG_LR,0x30000001)
        for reg,value in zip([UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3],args):
            self.uc.reg_write(reg,value)
        self.uc.emu_start(self.syms[name]|1,0x30000000,count=3000000)
        assert self.uc.reg_read(UC_ARM_REG_PC)==0x30000000,name
        return self.uc.reg_read(UC_ARM_REG_R0)
