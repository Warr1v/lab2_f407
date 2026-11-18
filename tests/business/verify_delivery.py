"""Check frozen ABI, module boundaries, ordinary-SRAM DMA and test-double isolation."""
from pathlib import Path
import hashlib
import json
import subprocess
import sys
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'build/test_python'))
from elftools.elf.elffile import ELFFile
import re
contract=(ROOT/'App/Contracts/business_api.h').read_bytes()
assert hashlib.sha256(contract).hexdigest()=='fe887cee7b05b7982d18f66b47238047337881e2e6802dd767b94c0f0887ec2a'
apis=re.findall(r'^(?:BizResult|BizMode|void) (\w+)\(',contract.decode(),re.M)
assert len(apis)==29
source_refs={
    'App/Wave':'e7a39eadb80bc81176baf88a160c5937945acda5',
    'App/Input':'84c2262c0b6af9a8adf1178a9eba27e994a60ed7',
    'App/SelfTest':'84c2262c0b6af9a8adf1178a9eba27e994a60ed7'}
for folder,revision in source_refs.items():
    for path in (ROOT/folder).glob('*'):
        if path.suffix not in ('.c','.h'):continue
        original=subprocess.check_output(['git','-C',str(ROOT),'show',f'{revision}:{path.relative_to(ROOT).as_posix()}'])
        assert path.read_bytes().replace(b'\r\n',b'\n')==original.replace(b'\r\n',b'\n'),path

def symbols(path):
    with path.open('rb') as f:
        elf=ELFFile(f)
        return {s.name:(s['st_value'],s['st_size']) for s in elf.get_section_by_name('.symtab').iter_symbols()}
for mode in ('Debug','Release'):
    syms=symbols(ROOT/f'build/{mode}/lab2_f407.elf')
    assert not any(n.startswith(('fake_','Fake_','test_checks','test_failure')) for n in syms)
    assert 'g_input_debug' not in syms
    for name in apis: assert name in syms,(mode,name)
    for name in ('rising','falling'):
        address,size=syms[name]
        assert 0x20000000<=address<0x20020000 and size==2048 and address%4==0,(name,address,size)
    assert 'g_wave_test_command' not in syms
    for name in ('ScreenApp_Init','Snake_Step','DMA1_Stream1_IRQHandler','DMA1_Stream2_IRQHandler',
                 'DMA1_Stream4_IRQHandler','TIM1_UP_TIM10_IRQHandler','TIM8_UP_TIM13_IRQHandler'):
        assert name in syms,name
print(json.dumps({'frozen_apis':len(apis),'backends_match_final_commits':True,
    'production':'real Wave/Input/Sweep, no test doubles or command mailboxes','dma_sram':'PASS'}))
