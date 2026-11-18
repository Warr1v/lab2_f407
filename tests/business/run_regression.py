"""Run original baseline checks without editing screen/game code.
The asset checker assumes the main checkout's parent contains reference files;
only its source lookup is rebound here for the deeper input worktree.
"""
from pathlib import Path
import runpy
import sys
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'build/test_python'))
check=ROOT/'BSP/Screen/tools/verify_assets.py'
text=check.read_text(encoding='utf-8')
original='source = project.parent / "2026创新实验室招新选拔软件第二题/第二题例程/HARDWARE/tftlcd"'
assert text.count(original)==1
source=ROOT.parents[2]/'2026创新实验室招新选拔软件第二题/第二题例程/HARDWARE/tftlcd'
exec(compile(text.replace(original,'source = Path('+repr(str(source))+')'),str(check),'exec'),
     {'__file__':str(check),'__name__':'__main__'})
runpy.run_path(str(ROOT/'tests/verify_snake.py'),run_name='__main__')
