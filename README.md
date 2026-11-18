# TerseTalk-CYD

A fast, compact **TerseTalk** bytecode virtual machine, Pythonic collection library, and overlapping/maximized windowing & math/graphics IDE targeting the bare-metal **ESP32 Cheap Yellow Display (`ESP32-2432S028R`, 320×240 ILI9341 + XPT2046 resistive touch + MicroSD + 4 MB Flash)**.

## Architecture Highlights

- **32-Bit 2-Bit-Tagged `Oop` (`uint32_t`)**:
  - `00` — 8-byte-aligned heap object pointer (`ObjSlots`, `ObjStr`, `ObjList`, `ObjDict`, `ObjCls`, `ObjMeth`, `ObjBlk`, `ObjFrame`, `ObjPen`, `ObjBmp`, `ObjWin`).
  - `01` — 30-bit signed `Int` (`-536,870,912 .. +536,870,911`; overflows auto-promote seamlessly to `Float`).
  - `10` — 30-bit immediate `Float` (IEEE-754 `float32` rounded-to-nearest-even into top 30 bits, decoded in 1 bitwise `AND` directly into the ESP32 hardware single-precision FPU with **zero heap allocation**).
  - `11` — Other immediates (`nil`, `up`, 21-bit `Char`, 27-bit interned lowercase `Sym`).
- **Aligned 32-Bit RAM Bytecodes**:
  - Two-pass recursive-descent TerseTalk compiler emitting fixed-width `uint32_t` instructions in RAM (`[op:8 | a:8 | bc:16]`), with inline-compiled `IF..THEN..ELIF..ELSE`, `WHILE..DO`, `FOR..DO` / `MAP` / `IF..MAP` / `INIT..REDUCE`, `COND`, `CASE..OF`, `AND`, `OR`, `MAYBE..NULL`, `TRY..CATCH`, `FN(...)` lexical closures, and monomorphic call-site inline caches.
- **No Wi-Fi / No Bluetooth**:
  - Reclaims >250 KB of ESP32 DRAM for a 144 KB coalescing mark-sweep GC arena, RAM bytecodes, and 8-bit `Bmp` offscreen pixel buffers.
- **Dual Filesystem**:
  - `/flash` (`LittleFS`, with `.rodata` embedded fallback) — immutable `prelude.tt` and built-in apps.
  - `/sd` (`SD` over hardware VSPI) — mutable scripts and live Smalltalk-style append-only world log `/sd/worlds/w_main.tt`.
- **Overlapping & Maximized `320×240` Desktop**:
  - **Overlapping mode**: Movable, SE-resizable windows (`y = 16..239`) with Z-order focus, `[Max]` (`^`), and 2-tap `[x]` close.
  - **Maximized mode**: Full `320×240` graphics canvas with `yak-cyd`-style downward triangle wedges in the **NW** (Global Menu) and **NE** (Current App Menu, populated dynamically by `currentApp appMenu`) corners.
  - **Input**: On-screen Stylus QWERTY keyboard, dual-pane **Palm Graffiti** unistroke recognizer, 1-tap TerseTalk symbol quick-bar (`:`, `$`, `@`, `=`, `.`, `,`, `;`, `'`, `(`, `)`, `me`, `IF`, `FOR`, `FN`), and `115200` baud USB Serial REPL (`:ls`, `:cat`, `:put`, `:load`, `:open`).

## Quick Start

```bash
# Run host unit & headless UI/graphics tests (generates 320x240 PPM screenshots in build/)
make test

# Build ESP32 CYD firmware & LittleFS flash image via PlatformIO
make build
make fs

# Flash CYD over USB & open 115200 Serial REPL
make upload
make uploadfs
make monitor
```

## Graphics & Math Examples (`Graphics Pad` / `Workspace`)

```tersetalk
"Plot sin(x) and cos(3x) in mathematical Cartesian world coordinates:"
p clear: 001 $ world: -6.3 @ -1.6 to: 6.3 @ 1.6 $ axes.
p yellow plot: FN(x: x sin).
p cyan plot: FN(x: (x * 3) cos * 0.6)

"Parametric 5-petal polar rose:"
p clear $ world: -1.2 @ -1.2 to: 1.2 @ 1.2 $ axes.
p magenta param: FN(t: r = (t * 5) cos. (r * t cos) @ (r * t sin)) from: 0 to: Num tau steps: 240

"Turtle rainbow spiral:"
p clear $ home $ down.
FOR(i: 90) DO(p hue: i * 4 $ go: i * 1.4 $ turn: 91)
```

## Documentation

- [TerseTalk-Alpha Language & Library Reference](doc/tersetalk-alpha-reference.md)
