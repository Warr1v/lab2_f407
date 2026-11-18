"""Execute the built Debug ARM firmware with mocked LCD/touch, without a board.

Dependencies: unicorn==2.1.4, pyelftools==0.32. Install normally or into
build/snake_checks/python. Run after building Debug: python tests/verify_snake.py
Only hardware boundary functions are mocked; game, UI, debounce and fonts come
from the actual firmware. This does not validate physical touch or LCD timing.
"""
from pathlib import Path
import json
import re
import struct
import sys
import zlib

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/snake_checks"
sys.path.insert(0, str(OUT / "python"))
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3
from unicorn.arm_const import UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC


class Firmware:
    def __init__(self):
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        for address, size in [(0x08000000, 0x100000), (0x20000000, 0x20000),
                              (0x40000000, 0x100000), (0x42000000, 0x2000000),
                              (0xE0000000, 0x100000), (0x30000000, 0x1000)]:
            self.uc.mem_map(address, size)
        with (ROOT / "build/Debug/lab2_f407.elf").open("rb") as file:
            elf = ELFFile(file)
            for segment in elf.iter_segments():
                if segment["p_type"] == "PT_LOAD" and segment["p_filesz"]:
                    self.uc.mem_write(segment["p_vaddr"], segment.data())
            self.syms = {s.name: (s["st_value"], s["st_size"])
                         for s in elf.get_section_by_name(".symtab").iter_symbols()}
            for cu in elf.get_dwarf_info().iter_CUs():
                for die in cu.iter_DIEs():
                    if die.tag == "DW_TAG_typedef" and die.attributes.get("DW_AT_name") and die.attributes["DW_AT_name"].value == b"SnakeState":
                        self.offsets = {child.attributes["DW_AT_name"].value.decode():
                                        child.attributes["DW_AT_data_member_location"].value
                                        for child in die.get_DIE_from_attribute("DW_AT_type").iter_children()}
                        break
                if hasattr(self, "offsets"):
                    break
        self.now, self.sample = 1000, (False, False, 0, 0)
        self.pixels = [0xFFFF] * (240 * 320)
        self.glyphs = {int(cp, 16): bytes(int(n, 16) for n in values.split(","))
                       for cp, values in re.findall(r"\{ 0x([0-9A-F]{4})U, \{ ([^}]+) \}",
                           (ROOT / "BSP/Screen/screen_font_cn.h").read_text(encoding="utf-8"))}
        self.hook("ScreenDelay_Init", lambda: None)
        self.hook("ScreenTouch_Init", lambda: None)
        self.hook("ScreenCal_SelfTest", lambda: self.uc.reg_write(UC_ARM_REG_R0, 0x7F))
        self.hook("HAL_GetTick", lambda: self.uc.reg_write(UC_ARM_REG_R0, self.now))
        self.hook("TFTLCD_Init", lambda: self.write("tftlcd_data", struct.pack("<HHHBB", 240, 320, 0x9341, 0, 0)))
        self.hook("ScreenTouch_Read", lambda: self.uc.mem_write(self.arg(0), struct.pack("<??HH", *self.sample)))
        self.hook("ScreenCal_Map", self.map_touch)
        self.hook("LCD_Clear", lambda: self.rect(0, 0, 239, 319, self.arg(0)))
        self.hook("LCD_Fill", lambda: self.rect(*(self.arg(i) for i in range(5))))
        self.hook("LCD_DrawRectangle", self.outline)
        self.hook("LCD_ShowFontHZ", self.chinese)
        self.hook("LCD_ShowString", self.ascii)

    def arg(self, i):
        if i < 4:
            return self.uc.reg_read([UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3][i])
        return int.from_bytes(self.uc.mem_read(self.uc.reg_read(UC_ARM_REG_SP) + (i-4)*4, 4), "little")

    def write(self, name, value):
        self.uc.mem_write(self.syms[name][0], value)

    def number(self, name, value=None):
        address, size = self.syms[name]
        if value is not None:
            self.uc.mem_write(address, value.to_bytes(size, "little"))
        return int.from_bytes(self.uc.mem_read(address, size), "little")

    def hook(self, name, callback):
        def intercept(uc, address, size, data):
            callback()
            uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
        address = self.syms[name][0] & ~1
        self.uc.hook_add(UC_HOOK_CODE, intercept, begin=address, end=address)

    def call(self, name, *args):
        self.uc.reg_write(UC_ARM_REG_SP, 0x2001FFF0)
        self.uc.reg_write(UC_ARM_REG_LR, 0x30000001)
        for i, value in enumerate(args):
            self.uc.reg_write([UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3][i], value)
        self.uc.emu_start(self.syms[name][0] | 1, 0x30000000, count=3000000)
        assert self.uc.reg_read(UC_ARM_REG_PC) == 0x30000000, f"{name} did not return"
        return self.uc.reg_read(UC_ARM_REG_R0)

    def state(self):
        data = self.uc.mem_read(self.syms["game"][0], self.syms["game"][1])
        return bytes(data[:120]), data[self.offsets["score"]], data[self.offsets["status"]], bool(data[self.offsets["automatic"]])

    def invariant(self):
        cells, score, status, _ = self.state()
        assert set(cells) <= {0, 1, 2, 3}
        assert cells.count(2) == 1 and cells.count(1) == score + 2
        assert cells.count(3) == (0 if status == 3 else 1)

    def map_touch(self):
        # Identity mapping isolates UI interaction from the already tested panel fit.
        self.uc.mem_write(self.arg(5), struct.pack("<h", self.arg(1)))
        self.uc.mem_write(self.arg(6), struct.pack("<h", self.arg(2)))
        self.uc.reg_write(UC_ARM_REG_R0, 1)

    def rect(self, x0, y0, x1, y1, color):
        assert 0 <= x0 <= x1 < 240 and 0 <= y0 <= y1 < 320, (x0, y0, x1, y1)
        for y in range(y0, y1+1):
            self.pixels[y*240+x0:y*240+x1+1] = [color] * (x1-x0+1)

    def outline(self):
        x0, y0, x1, y1 = [self.arg(i) for i in range(4)]
        color = self.number("FRONT_COLOR")
        for box in [(x0,y0,x1,y0), (x0,y1,x1,y1), (x0,y0,x0,y1), (x1,y0,x1,y1)]:
            self.rect(*box, color)

    def string(self, address):
        data = bytearray()
        while (byte := self.uc.mem_read(address+len(data), 1)) != b"\0":
            data.extend(byte)
        return data.decode("utf-8")

    def letters(self, x, y, value, size):
        fg, bg = self.number("FRONT_COLOR"), self.number("BACK_COLOR")
        for char in value:
            if ord(char) > 127:
                bits = self.glyphs[ord(char)]
                for dy in range(29):
                    for dx in range(32):
                        self.rect(x+dx,y+dy,x+dx,y+dy,fg if bits[dy*4+dx//8] & (0x80 >> (dx%8)) else bg)
                x += 32
            else:
                stride = (size+7)//8
                address = self.syms[f"ascii_{size}{size//2:02d}"][0] + (ord(char)-32)*stride*(size//2)
                bits = self.uc.mem_read(address, stride*(size//2))
                for dx in range(size//2):
                    for dy in range(size):
                        self.rect(x+dx,y+dy,x+dx,y+dy,fg if bits[dx*stride+dy//8] & (0x80 >> (dy%8)) else bg)
                x += size//2

    def chinese(self):
        self.letters(self.arg(0), self.arg(1), self.string(self.arg(2)), 16)

    def ascii(self):
        self.letters(self.arg(0), self.arg(1), self.string(self.arg(5)), self.arg(4))

    def scan(self, ms=8, pressed=False, x=0, y=0):
        self.now = (self.now + ms) & 0xFFFFFFFF
        self.sample = (pressed, pressed, x, y)
        self.call("ScreenApp_Process", self.now)

    def click(self, x, y, settle=True):
        self.scan(8, True, x, y)
        self.scan(8)
        self.scan(16)
        if settle:
            self.scan(8)

    def snapshot(self, name):
        def chunk(kind, data):
            return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind+data))
        rows = bytearray()
        for y in range(320):
            rows.append(0)
            for color in self.pixels[y*240:(y+1)*240]:
                rows.extend((((color>>11)&31)*255//31, ((color>>5)&63)*255//63, (color&31)*255//31))
        OUT.mkdir(parents=True, exist_ok=True)
        (OUT/name).write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB",240,320,8,2,0,0,0)) + chunk(b"IDAT",zlib.compress(rows)) + chunk(b"IEND",b""))


def main():
    fw = Firmware()
    fw.call("Snake_Reset", 0, 123)
    initial = fw.state()
    assert initial[2:] == (0, False)
    fw.call("Snake_Step")
    assert fw.state() == initial
    for invalid in [3, 255, 0xFFFFFFFF]:
        fw.call("Snake_SetDirection", invalid)
        assert fw.state() == initial  # reverse/invalid direction cannot start a game
    fw.call("Snake_SetDirection", 0)
    fw.call("Snake_SetDirection", 3)
    fw.call("Snake_Step")
    assert fw.state()[0].index(2) == 52  # second turn is ignored in the same step
    fw.call("Snake_SetRunning", 0)
    paused = fw.state()
    fw.call("Snake_SetDirection", 1)
    fw.call("Snake_Step")
    assert fw.state() == paused and paused[2] == 4
    fw.call("Snake_SetRunning", 1)
    for _ in range(6):
        fw.call("Snake_Step")
    assert fw.state()[2] == 2  # top wall
    lost = fw.state()
    fw.call("Snake_SetRunning", 1)
    fw.call("Snake_Step")
    assert fw.state() == lost

    # Ring-buffer wrap, legal movement into a departing tail, and body collision.
    for trail, expected in [([61,62,72,71], 1), ([60,61,62,72,71], 2)]:
        fw.call("Snake_Reset", 0, 123)
        cells = bytearray(120)
        for cell in trail[:-1]:
            cells[cell] = 1
        cells[trail[-1]], cells[119] = 2, 3
        fw.uc.mem_write(fw.syms["game"][0], bytes(cells))
        fw.uc.mem_write(fw.syms["game"][0]+fw.offsets["score"], bytes([len(trail)-3]))
        for name, value in [("length",len(trail)), ("tail_slot",118), ("head",71), ("direction",3), ("pending_direction",3)]:
            fw.number(name, value)
        for i, cell in enumerate(trail):
            fw.uc.mem_write(fw.syms["body"][0]+(118+i)%120, bytes([cell]))
        fw.call("Snake_SetDirection", 0)
        fw.call("Snake_Step")
        assert fw.state()[2] == expected
        fw.invariant()

    steps = 0
    for seed in [0,1,2,3,17,42,123,65535,65536,0x12345678,0x80000000,0xFFFFFFFF]:
        fw.call("Snake_Reset", 1, seed)
        for step in range(14041):
            if fw.state()[2] != 1:
                break
            fw.call("Snake_Step")
            steps += 1
            if step % 64 == 0:
                fw.invariant()
        fw.invariant()
        assert fw.state()[1:3] == (117, 3), (seed, step, fw.state()[1:])
        won = fw.state()
        fw.call("Snake_SetRunning", 1)
        fw.call("Snake_Step")
        assert fw.state() == won
    print(f"PASS: manual, direction validation, pause, wall/body/tail, ring wrap; 12 automatic wins in {steps} steps")

    # Use a fresh ELF image for UI/touch tests.
    fw = Firmware()
    fw.call("ScreenApp_Init")
    fw.scan()
    fw.click(180, 250, settle=False)
    assert fw.state()[2] == 0 and fw.number("button_count") == 8
    fw.snapshot("manual_ready.png")
    fw.click(200, 165)  # immediate next press is blocked by page-transition guard
    assert fw.state()[2] == 0
    fw.click(200, 165)  # right begins manual play
    assert fw.state()[2] == 1
    start = fw.state()[0]
    fw.scan(180)
    assert fw.state()[0] != start
    fw.click(180, 24)  # pause
    assert fw.state()[2] == 4
    paused = fw.state()
    fw.scan(2000)
    assert fw.state() == paused
    fw.click(180, 24)
    assert fw.state()[2] == 1
    fw.click(200, 290)  # return pauses and freezes the game
    paused = fw.state()
    assert paused[2] == 4
    fw.scan(5000)
    assert fw.state() == paused
    fw.click(180, 250)
    assert fw.state() == paused
    fw.click(120, 290)  # reset
    assert fw.state()[2:] == (0, False)
    fw.scan(8, True, 40, 290)  # hold mode; fires only on release
    for _ in range(20):
        fw.scan(8, True, 40, 290)
    assert not fw.state()[3]
    fw.scan(8)
    fw.scan(16)
    assert fw.state()[3]
    fw.scan(8, True, 40, 290)  # move out cancels mode change
    fw.scan(8, True, 100, 250)
    fw.scan(8)
    fw.scan(16)
    assert fw.state()[3]
    fw.snapshot("automatic_running.png")
    before = fw.state()
    fw.scan(10000)  # a delayed frame advances once, not 55 times
    assert fw.state()[0].index(2) == before[0].index(2)+1
    fw.click(180,24)
    assert fw.state()[2] == 4
    fw.now = 0xFFFFFFC0
    fw.click(180,24)
    assert fw.state()[2] == 1
    before = fw.state()
    fw.scan(180)
    assert fw.now < 200 and fw.state()[0] != before[0]
    fw.click(40,290)  # manual mode starts with a fresh ready board
    assert fw.state()[2:] == (0, False)
    fw.click(200,165)
    for _ in range(8):
        fw.scan(180)
    assert fw.state()[2] == 2
    fw.snapshot("manual_lost.png")
    lost = fw.state()
    fw.click(180,24)
    fw.click(200,165)
    assert fw.state() == lost  # terminal-state buttons are disabled
    fw.click(120,290)
    assert fw.state()[2] == 0
    fw.click(40,290)
    for _ in range(14041):
        if fw.state()[2] != 1:
            break
        fw.call("Snake_Step")
    fw.scan()
    assert fw.state()[1:3] == (117,3)
    fw.snapshot("automatic_won.png")
    fw.click(200,290)
    fw.click(60,190)  # existing stopwatch page still works
    fw.click(40,236)
    fw.scan(1000)
    assert fw.number("page") == 5
    assert fw.call("ScreenMock_Time", fw.syms["mock"][0], fw.now) >= 1000
    fw.snapshot("stopwatch_regression.png")
    print("PASS: real UI navigation, direction touch, pause/resume/reset, preserved progress, long hold, move-out cancellation, delayed frame and tick wrap")
    (OUT/"result.json").write_text(json.dumps({"automatic_seeds":12,"automatic_steps":steps,"core":"PASS","ui":"PASS","hardware":"not tested"}, indent=2))


if __name__ == "__main__":
    main()
