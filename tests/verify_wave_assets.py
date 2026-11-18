"""Run the unchanged asset checks with original resources resolved in a worktree."""
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
script = ROOT / 'BSP/Screen/tools/verify_assets.py'
for parent in ROOT.parents:
    if (parent / '2026创新实验室招新选拔软件第二题').is_dir():
        resource_parent = parent
        break
else:
    raise RuntimeError('Original reference resource directory not found')
# Original script assumes it is immediately next to the reference resources.
# Resolve ONLY that location in memory; all its assertions stay unchanged.
code = script.read_text(encoding='utf-8').replace(
    'source = project.parent /', 'source = resource_parent /')
exec(compile(code, str(script), 'exec'), {'__file__': str(script), 'resource_parent': resource_parent})
