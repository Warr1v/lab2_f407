"""Check UI glyph coverage and source-resource preservation before delivery."""
from pathlib import Path
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
source = project.parent / "2026创新实验室招新选拔软件第二题/第二题例程/HARDWARE/tftlcd"
manifest = json.loads((project / "BSP/Screen/reference_sha256.json").read_text())
for name, digest in manifest.items():
    assert hashlib.sha256((source / name).read_bytes()).hexdigest() == digest, name
assert "AT24CXX_" not in (project / "BSP/Screen/touch.c").read_text()
assert "GPIO_InitTypeDef_SPL" not in (project / "BSP/Screen/tftlcd.c").read_text(encoding="utf-8")
print(f"PASS: {len(required)} UI characters covered by {len(glyphs)} valid glyphs.")
print("PASS: original LCD resources unchanged; no EEPROM/legacy SPL touch path.")
