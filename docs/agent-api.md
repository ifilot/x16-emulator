# X16 AgentBridge API, protocol version 1

X16 AgentBridge is a fork of the Commander X16 emulator for automated application
and agent development. The executable remains `x16emu`. The normal SDL frontend
and the legacy `-testbench` remain available.

## Build and launch

Required on Ubuntu: a C/C++ compiler, CMake >= 3.15, Python 3, `libsdl2-dev`, and
`zlib1g-dev`. FluidSynth is optional. No JSON or Python packages are required.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
./build/x16emu -agent -rom ../emulator/rom.bin -fsroot ./runtime
```

A compatible system ROM must be provided separately. `-agent` starts paused at
the CPU reset vector, before boot. Advance the machine to boot it. CLI `-prg`,
`-bas`, `-run`, `-sdcard`, `-fsroot`, CPU model, and clock options still apply.
The mode cannot be combined with `-testbench`, `-debug`, `-gif`, or `-wav`.

Agent mode opens no window, renderer, texture, game controller, or host audio
output. It retains CPU/device stepping, VERA scan timing, raster and collision
interrupts, PCM FIFO consumption, and YM synthesis/timers. Outside capture, it
skips layer rendering and framebuffer composition while
retaining scan timing, register history, and sprite processing for collision IRQs.
During capture it generates pixels into the existing CPU framebuffer, without
texture upload or presentation. There is no wall-clock pacing. Warp frame
skipping is suppressed during capture. RAM and VRAM start deterministically
rather than
using randomized power-on data. `-rtc` explicitly opts into host-time RTC setup.
HostFS retains upstream's interception behavior; use an SD image for tests that
need hardware filesystem timing.

## Transport

Send one UTF-8 JSON object per line on stdin. Receive exactly one response per
request on stdout, in order. Guest output, echo, traces, and diagnostics go to
stderr. There is no unsolicited greeting: send `hello` first. Drain stderr if
redirecting it to a pipe. EOF or `quit` terminates the process cleanly.

Requests are **flat objects**: fields may contain strings, exact integer numbers,
booleans, or null. Nested objects, arrays, floats, duplicate fields, unknown fields,
invalid UTF-8, and NUL characters are rejected. `id` is optional and must be an
integer in the JSON-safe range or null. All other integers are decimal JSON
numbers; hex byte payloads are strings. The maximum request length is 65,536
bytes excluding newline. Overlong requests are drained before processing the
next line. Strings support JSON escapes and Unicode surrogate pairs.

```json
{"id":1,"cmd":"hello"}
{"id":1,"ok":true,"result":{"name":"X16 AgentBridge","protocol":1,"paused":true,"max_request_bytes":65536,"max_memory_bytes":16384,"max_cycles":100000000,"commands":["hello","status","set_registers","memory_read","memory_write","load_prg","reset","advance","breakpoint","key","mouse","paste","screenshot","quit"]}}
```

Failures use `{"id":1,"ok":false,"error":"message"}`. Validation failures leave
validated input changes unapplied. File I/O failures and execution interrupts
are distinct from input validation; screenshot encoding can fail after advancing.
Only one request executes at a time. The machine sleeps waiting for the next
command, rather than running freely in the background.

## Commands

All commands use the `cmd` field and optionally `id`.

| Command | Additional fields | Behavior |
| --- | --- | --- |
| `hello` | none | Identify protocol and limits; return supported command names. |
| `status` | none | Read CPU registers, selected banks, waiting/halted flags, and session counters. |
| `set_registers` | Any of `pc,a,x,y,sp,p,dp,db,k,e,ram_bank,rom_bank` | Validate the entire update, then apply. Setting PC clears halt/WAI. |
| `memory_read` | `address`, optional `length=1`, `space="cpu"`, `bank=0` | Side-effect-free debug reads; result contains a hex `data` string. |
| `memory_write` | `address,data`, optional `space="cpu"`, `bank=0` | Validate the entire hex payload/range first; write bytes. |
| `load_prg` | `path`, optional `address`, `set_pc=false` | Load a two-byte-address PRG into low RAM. Result contains address and payload length. |
| `reset` | none | Soft machine reset; preserve RAM and session counters and breakpoints. Reset device/queued input state. |
| `advance` | Optional `unit="frames"`, `count=1`, `max_cycles=100000000` | Execute a bounded interval; return paused. |
| `breakpoint` | Optional `action="add"`, `address`, `bank=-1` | Add/remove/list/clear PC breakpoints; return the table. |
| `key` | `key,down` | Queue a physical key press/release. `key` is an SDL name, e.g. `A`, `1`, `Return`, `Left Shift`, `Escape`. |
| `mouse` | Optional `dx=0,dy=0,buttons=0,wheel=0` | Queue one relative mouse packet; positive Y moves down. |
| `paste` | `text` | Paste UTF-8 through the existing KERNAL keyboard buffer mechanism. |
| `screenshot` | `path`, optional `max_cycles=100000000` | Advance two VGA frame boundaries (three NTSC field boundaries), then write a fresh 640x480 RGB PNG. |
| `quit` | none | Reply then shut down cleanly. |

### Registers and counters

`status` returns `pc,a,x,y,sp,p,dp,db,k,e,c816,ram_bank,rom_bank,waiting,halted,
cycles,instructions,frames,mhz`. `a` represents the full accumulator C (including B)
in 65C816 mode; `p` is the processor status byte; `k` and `db` are processor
program/data banks. `ram_bank` and `rom_bank` are the X16 bank selectors.
65C02 A/X/Y are 8-bit. Emulation mode constrains SP to page 1 and index width to
8-bit. Native 65C816 register updates obey the index width flag. Updating X16
bank selectors also updates their zero-page RAM readback. Supported CPU program
banks follow upstream's models: nonzero K is supported in `-gs` mode.

Counters are 64-bit session totals since startup and survive soft resets.
Execution deltas include emulated cycles and executed instructions; idle WAI
cycles do not count as instructions. HostFS interceptions do not count as CPU
instructions. Debug reads do not advance the machine. API bus-write overhead is
not charged as execution. A soft reset is not a full snapshot restore or fresh
power-on; launch a new process for a clean RAM/filesystem baseline.

### Memory spaces

| Space | Address interpretation | Bank interpretation | Write semantics |
| --- | --- | --- | --- |
| `cpu` | CPU-visible address 0..65535 | CPU physical bank, normally 0; nonzero requires `-gs` | Uses the actual bus, including I/O and bank-selector side effects. Low-bank ROM writes are rejected. |
| `ram` | Raw RAM offset; bank 0 ends at $9EFF | Physical RAM bank, installed banks only | Direct backing memory, no I/O or bank-selector effects. |
| `banked_ram` | Offset 0..8191 | Explicit X16 RAM bank, independent of current selection | Direct backing memory. |
| `rom` | Offset 0..16383 | Explicit built-in ROM bank 0..31 | Read-only. |
| `vram` | Offset 0..131071 | Must be 0 | VERA internal writes update palette, sprite, and PSG state as appropriate. |

Read/write transfers are 1..16,384 bytes. Ranges cannot wrap or cross the space's
end. CPU reads observe currently selected X16 banks; use `banked_ram`/`rom` for
explicit-bank inspection without changing selectors. Cartridge ROM inspection
is possible through the CPU view after selecting its bank; raw cartridge
storage is not a separate API space in version 1.

PRG loading accepts only payloads fitting into low RAM at $0002..$9EFF. Files
are read and validated before applying changes. `set_pc=true` jumps to the load
address; it does **not** interpret a BASIC stub or initialize a C runtime.
For normal BASIC-stub programs use CLI `-prg ... -run`, or boot and paste a LOAD/RUN
command. For machine-code programs, set PC to the intended entry label.

### Execution, breakpoints, and capture

`unit` may be `cycles`, `instructions`, or `frames`. Both `count` and
`max_cycles` must be 1..100,000,000. Instructions are indivisible: cycle targets
and limits can overshoot by the final instruction and interrupt entry.
When the requested target is reached, `reason` is `target`; if the cycle budget
runs out first it is `cycle_limit`. Other reasons are `breakpoint`, `stp`, `exit`
(PC $FFFF), and `poweroff`. These halt events retain the API; reset or set PC to
resume. The native STP path does not open a dialog in agent mode.

Execution returns:

```json
{"id":2,"ok":true,"result":{"reason":"target","cycles":6,"instructions":3,"frames":0,"captured":false,"state":{"pc":24579,"a":0,"x":0,"y":0,"sp":509,"p":52,"dp":0,"db":0,"k":0,"e":1,"c816":false,"ram_bank":0,"rom_bank":0,"waiting":false,"halted":false,"cycles":6,"instructions":3,"frames":0,"mhz":8}}}
```

Breakpoints stop **before** executing the matching PC. `bank=-1` matches any
bank. At $A000..$BFFF the bank is the selected X16 RAM bank, at $C000..$FFFF it is
the X16 ROM bank, and below $A000 it is 0. In nonzero `-gs` processor banks it is
the physical CPU bank. Maximum 32 breakpoints; adding the same pair is idempotent.
A breakpoint already at the current PC stops immediately. Remove it before
continuing. IRQ handler PCs can be breakpoint targets as well.

`screenshot` deliberately advances the machine: the first boundary closes a
possibly partial frame and the second completes a new frame. NTSC capture uses
three boundaries to finish both complete fields after an initial partial field.
This preserves
raster effects from real emulated execution. No stale warp-skipped framebuffer
is returned. Breakpoints, STP, or a cycle limit interrupt capture with
`captured=false` and no file written. Check `captured` before opening the file.
A bad output path produces an API error **after** the capture frames have elapsed.
Existing files at a successful capture path are overwritten. Paths refer to the
host filesystem and are relative to the emulator's working directory, not HostFS.

### Input sequencing

The keyboard queue holds 15 key codes; the mouse queue holds up to three current
four-byte packets. Full queues produce errors instead of silently dropping
input. Advance the machine to let its KERNAL consume pending packets. Mouse
`dx`/`dy` are -255..255; wheel is -7..7. Button bits are 1=left, 2=right, 4=middle.
Each mouse command supplies the complete button mask (default releases all).
Absolute guest cursor placement depends on the guest's mouse configuration;
version 1 provides relative device movement.

Press and release are separate commands. Advance between them so guest handlers
can observe both edges. Mouse movement/button/wheel state is emitted in one
packet. Reset clears queues and mouse state. Paste is ROM-dependent, uses the
existing ROM keyboard-buffer symbols, and rejects a second paste while active.

## Python client

`api/python/x16agent.py` uses only Python's standard library. It provides a
context manager, request IDs, response timeouts, and memory/advance/capture
helpers. A response timeout kills the subprocess to avoid consuming a stale
response on a subsequent command. Pass stderr as an inherited stream or writable
file, and keep runtime/test files separate from source assets.

```python
import sys
sys.path.insert(0, "api/python")
from x16agent import X16Agent

with X16Agent("./build/x16emu", "../emulator/rom.bin", fsroot="./runtime",
              options=["-prg", "./runtime/APP.PRG", "-run"]) as emu:
    emu.advance(120)  # boot and let the application initialize
    emu.request("key", key="1", down=True)
    emu.advance(2)
    emu.request("key", key="1", down=False)
    result = emu.screenshot("./screen.png")
    if result["captured"]:
        print("Captured", result["state"]["frames"])
```

Use named PC breakpoints from linker labels instead of fixed boot delays for
application tests. A separate driver can expose this subprocess interface to an
agent tool system; no browser or network server is required by the emulator.

## Verification

```sh
ctest --test-dir build --output-on-failure
cmake -S . -B build-sanitize -DCMAKE_BUILD_TYPE=Debug \
  -DENABLE_LTO=OFF -DENABLE_SANITIZERS=ON \
  -DENABLE_FLUIDSYNTH=OFF -DENABLE_TRACE=OFF
cmake --build build-sanitize --parallel 4
ctest --test-dir build-sanitize --output-on-failure
```

The C tests exercise JSON parsing (including malformed-input fuzzing) and actual
keyboard/mouse packet generation. Python integration tests launch real emulator
processes with generated ROMs: CPU execution, flag/register widths, bounded WAI,
breakpoints, memory isolation/ranges, atomic validation, PRG loading, VERA IRQs,
PNG dimensions/CRC/pixels/freshness, silent PCM/YM timing, queue backpressure,
stdout isolation, timeout cleanup, EOF, and legacy testbench regressions. No
proprietary ROM, network download, desktop, or Python imaging package is needed.

Full machine snapshot/restore and a network/MCP server are not implemented in
protocol version 1. Pixel composition is already limited to capture intervals;
sprite work needed for collision IRQs remains active between captures.

LeakSanitizer requires process tracing permissions. In environments that reject
its ptrace scan, run with `ASAN_OPTIONS=detect_leaks=0:halt_on_error=1`; AddressSanitizer
and UndefinedBehaviorSanitizer remain active. CI enables leak checks on its native runner.
