"""Create reviewed HAL working copies; never modify the teaching resources."""
from pathlib import Path
import hashlib
import json
import re

project = Path(__file__).resolve().parents[3]
dest = project / "BSP/Screen"
source = project.parent / "2026创新实验室招新选拔软件第二题/第二题例程/HARDWARE/tftlcd"

def replace_function(text, name, replacement):
    match = re.search(r"(?m)^(?:void|u8|u16|u32) " + name + r"\([^;]*?\)\s*\{", text)
    if not match:
        raise RuntimeError(name)
    start = text.index("{", match.start())
    level = 1
    end = start + 1
    while level:
        level += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[:match.start()] + replacement + text[end:]

c = (source / "tftlcd.c").read_text(encoding="gbk")
h = (source / "tftlcd.h").read_text(encoding="gbk")
font = (source / "font.H").read_text(encoding="gbk")
c = c.replace('#include "usart.h"', '/* UART intentionally not required. */')
c = replace_function(c, "TFTLCD_GPIO_Init", "/* GPIO owned by CubeMX MX_GPIO_Init / MX_FSMC_Init. */")
c = replace_function(c, "TFTLCD_FSMC_Init", "/* FSMC bank4 and timing owned by CubeMX. */")
c = c.replace("\tTFTLCD_GPIO_Init();", "").replace("\tTFTLCD_FSMC_Init();", "")
c = c.replace("\tu16 i;\n\n\n", "\tu16 i __attribute__((unused));\n\n")
c = c.replace("\tu16 r1,r2,r3;", "\tu16 r1 __attribute__((unused)), r2 __attribute__((unused)), r3 __attribute__((unused));")
c = c.replace("\tu32 value;", "\tu32 value __attribute__((unused));")
c = re.sub(r"(?m)^\s*printf\([^\n]*", "\n\t/* No blocking console output in the screen driver. */", c)
c = c.replace("tftlcd_data.id|=(tftlcd_data.id<<8)|TFTLCD->LCD_DATA;", "tftlcd_data.id=(tftlcd_data.id<<8)|(TFTLCD->LCD_DATA & 0xFFU);")
h = h.replace("((TFTLCD_TypeDef *) TFTLCD_BASE)", "((volatile TFTLCD_TypeDef *) TFTLCD_BASE)")
c = replace_function(c, "LCD_Clear", """void LCD_Clear(u16 color)
{
    LCD_Fill(0, 0, tftlcd_data.width - 1, tftlcd_data.height - 1, color);
}""")
# Discard both legacy HZ implementations (including the inactive 20-entry loop).
a = c.index("#if 0\nvoid LCD_ShowFontHZ")
b = c.index("void LCD_ShowPicture", a)
c = c[:a] + r'''/* UTF-8 text; generated Unicode indices and 32x29 bitmaps.
 * Same LCD window/data primitives as the supplied HZ function, bounded lookup.
 */
void LCD_ShowFontHZ(u16 x, u16 y, u8 *text)
{
    const u8 *p = text;
    while (*p && x < tftlcd_data.width) {
        uint32_t cp;
        if (*p < 0x80) {
            if (x + 8 > tftlcd_data.width || y + 16 > tftlcd_data.height) return;
            if (*p >= 32 && *p <= 126) LCD_ShowChar(x, y, *p, 16, 0);
            x += 8; p++; continue;
        }
        if ((*p & 0xF0) != 0xE0 || !p[1] || !p[2]
            || (p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80) return;
        cp = ((p[0] & 0x0FU) << 12) | ((p[1] & 0x3FU) << 6) | (p[2] & 0x3FU);
        if (x + 32 > tftlcd_data.width || y + 29 > tftlcd_data.height) return;
        const ScreenGlyph *glyph = NULL;
        for (unsigned k = 0; k < sizeof(screen_glyphs) / sizeof(screen_glyphs[0]); ++k) {
            if (screen_glyphs[k].codepoint == cp) { glyph = &screen_glyphs[k]; break; }
        }
        LCD_Set_Window(x, y, x + 31, y + 28);
        for (unsigned n = 0; n < 928; ++n) {
            const int ink = glyph ? (glyph->bits[n / 8] & (0x80U >> (n % 8))) :
                (n / 32 == 0 || n / 32 == 28 || n % 32 == 0 || n % 32 == 31);
            LCD_WriteData_Color(ink ? FRONT_COLOR : BACK_COLOR);
        }
        x += 32; p += 3;
    }
}

''' + c[b:]
# Keep the original ASCII tables. Replace GBK-specific HZ table with generated glyphs.
font = font[:font.index("// ------------------")]
font += '\n#include "screen_font_cn.h"\n#endif\n'
for name, text in (("tftlcd.c", c), ("tftlcd.h", h), ("font.h", font)):
    (dest / name).write_text(text, encoding="utf-8", newline="\n")
manifest = {name: hashlib.sha256((source / name).read_bytes()).hexdigest()
            for name in ("tftlcd.c", "tftlcd.h", "font.H")}
(dest / "reference_sha256.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
print("Created HAL LCD working copies; original resource hashes recorded.")
