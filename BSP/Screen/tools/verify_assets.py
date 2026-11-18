"""Check UI glyph coverage and source-resource preservation before delivery."""
from pathlib import Path
import argparse
import hashlib
import json
import re

project = Path(__file__).resolve().parents[3]
app = (project / "App/Screen/screen_app.c").read_text(encoding="utf-8")
required = {c for s in re.findall(r'"([^"\n]*)"', app) for c in s if ord(c) > 127}
header = (project / "BSP/Screen/screen_font_cn.h").read_text(encoding="utf-8")
glyphs = re.findall(r'\{ 0x([0-9A-F]{4})U, \{ ([^}]+) \}', header)
available = {chr(int(codepoint, 16)) for codepoint, _ in glyphs}
assert not required - available, f"Missing Chinese glyphs: {required - available}"
for codepoint, values in glyphs:
    assert len(values.split(',')) == 116, f"Wrong bitmap length: {codepoint}"
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--reference-dir", type=Path, help="Optional original teaching LCD resource directory")
parser.add_argument("--require-reference", action="store_true", help="Fail if original teaching resources are unavailable")
args = parser.parse_args()
source = args.reference_dir or project.parent / "2026创新实验室招新选拔软件第二题/第二题例程/HARDWARE/tftlcd"
manifest = json.loads((project / "BSP/Screen/reference_sha256.json").read_text())
if source.is_dir():
    for name, digest in manifest.items():
        assert hashlib.sha256((source / name).read_bytes()).hexdigest() == digest, name
    print("PASS: original LCD resource hashes unchanged.")
else:
    if args.require_reference:
        raise FileNotFoundError(f"Original LCD resources not found: {source}")
    print("SKIP: optional original teaching resources unavailable; glyph/adaptation checks still run.")
assert "AT24CXX_" not in (project / "BSP/Screen/touch.c").read_text()
assert "GPIO_InitTypeDef_SPL" not in (project / "BSP/Screen/tftlcd.c").read_text(encoding="utf-8")
print(f"PASS: {len(required)} UI characters covered by {len(glyphs)} valid glyphs.")
print("PASS: no EEPROM/legacy SPL touch path.")
