$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$screenDir = Split-Path -Parent $PSScriptRoot
$labels = '屏幕调试原始坐标有效无按下移动抬起取消校准长清除菜单功能波输出输入检测秒表模拟返回开启关闭运行暂停继续开始重置记圈段记录成功失败超时未完成请点十字第个松手重退出保存在内存频率占空比信号等待已位边缘中心颜色测试仅提示'
$glyphChars = @($labels.ToCharArray() | Sort-Object -Unique)
$utf8 = [Text.UTF8Encoding]::new($false)
$builder = [Text.StringBuilder]::new()
[void]$builder.AppendLine('/* Generated from Windows SimSun, 28 px; 32x29 row-major, MSB first. */')
[void]$builder.AppendLine('typedef struct { uint32_t codepoint; uint8_t bits[116]; } ScreenGlyph;')
[void]$builder.AppendLine('static const ScreenGlyph screen_glyphs[] = {')
$font = [Drawing.Font]::new('SimSun', 28, [Drawing.FontStyle]::Regular, [Drawing.GraphicsUnit]::Pixel)
$format = [Drawing.StringFormat]::GenericTypographic.Clone()
$format.FormatFlags = $format.FormatFlags -bor [Drawing.StringFormatFlags]::NoClip
$preview = [Drawing.Bitmap]::new(32 * 12, 29 * [int][Math]::Ceiling($glyphChars.Count / 12.0))
$pg = [Drawing.Graphics]::FromImage($preview)
$pg.Clear([Drawing.Color]::White)
$index = 0
foreach ($ch in $glyphChars) {
    $bmp = [Drawing.Bitmap]::new(32,29)
    $g = [Drawing.Graphics]::FromImage($bmp)
    $g.Clear([Drawing.Color]::White)
    $g.TextRenderingHint = [Drawing.Text.TextRenderingHint]::SingleBitPerPixelGridFit
    $g.DrawString([string]$ch, $font, [Drawing.Brushes]::Black, [single]2, [single]0, $format)
    $bytes = [Collections.Generic.List[string]]::new()
    for ($y=0; $y -lt 29; $y++) { for ($group=0; $group -lt 4; $group++) {
        $value = 0
        for ($bit=0; $bit -lt 8; $bit++) {
            if ($bmp.GetPixel($group*8+$bit,$y).R -lt 128) { $value = $value -bor (0x80 -shr $bit) }
        }
        $bytes.Add(('0x{0:X2}' -f $value))
    } }
    [void]$builder.AppendLine(('  {{ 0x{0:X4}U, {{ {1} }} }}, /* {2} */' -f [int]$ch, ($bytes -join ','), $ch))
    $pg.DrawImageUnscaled($bmp, ($index % 12)*32, [int][Math]::Floor($index/12)*29)
    $index++
    $g.Dispose(); $bmp.Dispose()
}
[void]$builder.AppendLine('};')
[IO.File]::WriteAllText((Join-Path $screenDir 'screen_font_cn.h'), $builder.ToString(), $utf8)
$preview.Save((Join-Path $screenDir 'tools/font_preview.png'), [Drawing.Imaging.ImageFormat]::Png)
$pg.Dispose();$preview.Dispose();$format.Dispose();$font.Dispose()
Write-Output ('Generated {0} Chinese glyphs.' -f $glyphChars.Count)
