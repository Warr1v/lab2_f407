"""Check frozen ABI, module boundaries, ordinary-SRAM DMA and test-double isolation."""
from pathlib import Path
import hashlib
import json
import subprocess
import sys
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'build/test_python'))
from elftools.elf.elffile import ELFFile
BASE='0df814ca1bb0c6a713d38eae72504c2ba12749af'
template=ROOT.parents[2]/'ai_tasks/contracts/business_api.h'
assert template.read_bytes()==(ROOT/'App/Contracts/business_api.h').read_bytes()
changed=subprocess.check_output(['git','-C',str(ROOT),'diff','--name-only',BASE],text=True).splitlines()
changed+=subprocess.check_output(['git','-C',str(ROOT),'ls-files','--others','--exclude-standard'],text=True).splitlines()
changed=sorted(set(changed))
for name in changed:
    assert name.startswith(('App/Input/','App/SelfTest/','App/Contracts/','tests/business/')) or name in (
        'CMakeLists.txt','Core/Src/main.c','Core/Src/stm32f4xx_it.c','Core/Inc/stm32f4xx_it.h'),name
def symbols(path):
    with path.open('rb') as f:
        elf=ELFFile(f)
        return {s.name:(s['st_value'],s['st_size']) for s in elf.get_section_by_name('.symtab').iter_symbols()}
for mode in ('Debug','Release'):
    syms=symbols(ROOT/f'build/{mode}/lab2_f407.elf')
    assert not any(n.startswith(('fake_','Fake_','test_checks','test_failure')) for n in syms)
    assert 'g_input_debug' not in syms
    for name in ('Input_Init','Input_Process','Input_BeginWindow','Input_GetSnapshot',
                 'Input_SetMode','Input_GetMode','Input_GetCapabilities'):
        assert name in syms,name
    for name in ('rising','falling'):
        address,size=syms[name]
        assert 0x20000000<=address<0x20020000 and size==2048 and address%4==0,(name,address,size)
    assert 'Wave_Init' not in syms and 'Sweep_Start' not in syms
integrated=symbols(ROOT/'build/Integration/lab2_f407.elf')
assert not any(n.startswith(('fake_','Fake_','test_checks','test_failure')) for n in integrated)
for name in ('Wave_Init','Input_Init','Sweep_Init','Sweep_Process','Sweep_Start','Sweep_Cancel','Sweep_GetSnapshot'):
    assert name in integrated,name
print(json.dumps({'contract_sha256':hashlib.sha256(template.read_bytes()).hexdigest(),
                  'modified_paths':changed,'default_images':'no test doubles or output initialization',
                  'integrated_image':'real Wave/Input/Sweep, no test doubles','dma_sram':'PASS'}))
