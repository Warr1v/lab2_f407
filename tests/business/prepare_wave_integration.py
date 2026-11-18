"""Extract an explicit committed Wave tree read-only, then build an isolated image.
No checkout/merge/cherry-pick, no writes to Wave or main worktrees.
Usage after selecting the output commit: python .../prepare_wave_integration.py SHA
"""
from pathlib import Path
import hashlib
import json
import subprocess
import sys
ROOT=Path(__file__).resolve().parents[2]
def git(*args): return subprocess.check_output(['git','-C',str(ROOT),*args])
def prepare(sha):
    full=git('rev-parse',sha+'^{commit}').decode().strip()
    if sha!=full: raise ValueError('Use the full explicit reviewed commit SHA')
    target=ROOT/'build'/('wave_snapshot_'+full)
    manifest={'sha':full,'origin_branch':'feature/wave-output','files':{}}
    files=git('ls-tree','-r','--name-only',full,'App/Wave').decode().splitlines()
    for name in files:
        if not name.startswith('App/Wave/') or '..' in Path(name).parts: raise ValueError(name)
        content=git('show',full+':'+name)
        dest=target/name
        if dest.exists() and dest.read_bytes()!=content: raise ValueError('Changed cached snapshot: '+name)
        dest.parent.mkdir(parents=True,exist_ok=True)
        dest.write_bytes(content)
        manifest['files'][name]=hashlib.sha256(content).hexdigest()
    if not files: raise ValueError('No App/Wave files in selected commit')
    target.joinpath('manifest.json').write_text(json.dumps(manifest,indent=2))
    sources=';'.join(str(target/'App/Wave'/name) for name in ['wave.c','wave_math.c','wave_hw.c','spwm.c'])
    toolchain=[] if (ROOT/'build/Integration/CMakeCache.txt').exists() else [
        '-DCMAKE_TOOLCHAIN_FILE='+str(ROOT/'cmake/gcc-arm-none-eabi.cmake')]
    subprocess.run(['cmake','-S',str(ROOT),'-B',str(ROOT/'build/Integration'),'-G','Ninja',*toolchain,
                    '-DCMAKE_BUILD_TYPE=Debug','-DLAB2_ENABLE_SWEEP=ON',
                    '-DLAB2_ENABLE_INPUT_DEBUG=ON','-DLAB2_WAVE_SOURCES='+sources,
                    '-DLAB2_WAVE_INCLUDE_DIRS='+str(target/'App/Wave')],check=True)
    subprocess.run(['cmake','--build',str(ROOT/'build/Integration')],check=True)
    return manifest
if __name__=='__main__': prepare(sys.argv[1])
