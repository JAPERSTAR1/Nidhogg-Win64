# Initial extraction and reverse-engineering notes

Historical snapshot; not the current feature status. Original paths below are
from the investigation machine.

Native Windows x64 port work for **Nidhogg** (PS4, `UP2143-CUSA00920`, v1.00).

Source dump: `C:\Users\puy12\Downloads\g\CUSA00920`.

## State

| step | status |
|---|---|
| SELF → ELF extraction | done, `work/nidhogg.elf` |
| image survey (segments, code, relocs, imports) | done, `analysis/` |
| Windows host that maps and executes the image | done, `port/out/nidhogg_host.exe` |
| relocations applied | 7,972 |
| import slots found and stubbed | 465 |
| entry point | **broken in the dump**, must be recovered |
| SCE shim layer | not started |
| renderer (GNM → D3D11) | not started |

## What the dump actually contains

The eboot is a SELF wrapping an `ET_SCE_RELEXEC` ELF. The code segments are
plaintext x86-64, MSVC-built, with RTTI and the original build paths intact:

```
C:/Projects/FOCAL/Nidhogg/ps4/Nidhogg.elf
C:\Projects\FOCAL\FOCAL\ps4\savedata.cpp
```

Image layout (vaddr = file offset − 0x4000):

```
LOAD 0x000000  0x1ecf58  R+X   code
LOAD 0x1f0000  0x22ec48  RW    data + bss
PLT  0x16f310            465 entries, slots ascending from 0x33b2d8
RELA 0x345980..0x374a80  7,972 R_X86_64_RELATIVE records
```

Three independent structures agree on that mapping (PLT slot arithmetic,
relocation offsets, `lea` targets of the game's own string references), so the
image map is not guesswork.

**The dump is truncated.** The file ends at 0x377081 while its own program
headers declare data out to 0x37bd51 — 19,664 bytes of dynlib metadata are
missing (the dynamic table, part of the SCE string/symbol tables, and the
process-parameter block). The loadable image is complete; only symbol naming
suffers. Import identity has to come from call-site behaviour instead of NIDs.

## Tooling

```
tools/ps4_self.py    SELF → ELF, verifies the program headers and reports gaps
tools/analyze.py     code survey, function candidates, strings, import slots
tools/nids.py        import/library inventory from the dynlib segment
port/build.ps1       MSVC build
```

## Host

`port/src/image.cpp` parses the ELF, reserves one contiguous Windows region,
copies the segments, applies the relative relocations recovered from the RELA
table, and writes a generated thunk into each import slot.

`port/src/thunks.cpp` emits, per slot, a stub that records the call and returns
zero. Nothing is hard-coded to a symbol yet; the stubs are the scaffolding the
real shims drop into.

Run:

```
port\out\nidhogg_host.exe work\nidhogg.elf              # enter the game
port\out\nidhogg_host.exe work\nidhogg.elf --call 0x4dd0  # smoke test
```

The smoke test returns 7 — the mapped image executes. Entering the real entry
faults immediately because the header's `e_entry` (0x889d0) lands three bytes
into another function's immediate, not on a function boundary. That is a
property of this dump, not of the mapping: `e_entry` has to be recovered from
the code (the process-param block that would name it is in the missing tail).

## Port architecture

The PS4 build is native x86-64 with a System V ABI. Nothing is gained by
decompiling three thousand functions to C and recompiling them — the machine
code is already the program. The port is therefore a platform replacement:

```
 game image (unchanged, mapped)
      │
      ├── libSceGnmDriver / Gnmx / GpuAddress   → D3D11
      ├── libSceVideoOut                        → swapchain
      ├── libSceAudioOut / Ngs2                 → WASAPI (FMOD bank reused)
      ├── libScePad                             → XInput
      ├── libSceFios2                           → Win32 file IO
      ├── libSceSaveData / SaveDataDialog       → %APPDATA%
      ├── libSceUserService / Rtc / SystemService
      └── libSceNp* / Http / Ssl                → stubbed out
```

`libSceSysmodule` gates most of the others: it is the first thing the CRT calls
and every module load must succeed or the game bails.

The only genuinely hard piece is GNM, because it is a command-buffer API:
the game encodes PM4 packets rather than issuing draw calls. Two ways to land
it, and they are not interchangeable:

1. **PM4 interpreter** — parse the command buffer and translate packets into
   D3D11 state. Faithful, keeps the original renderer and shaders (which must
   then be translated from GCN ISA to HLSL). Weeks of work, but the game's
   visuals come out as built.
2. **Draw-level interception** — hook the engine's own sprite/quad submit path
   and reimplement it in D3D11. Much smaller; throws away the original
   renderer, so every effect has to be rebuilt by hand.

## Next block of work

1. Recover the entry point: scan for the CRT startup, cross-check against the
   Sysmodule calls it makes, wire `Sysmodule` to a table of fake module ids.
2. Get the process to the first `sceGnmDriver` call and log the command stream.
3. Classify the 465 import slots by call site (format strings, buffer sizes,
   return-code checks) until each one has a name.
4. Stand up D3D11 behind `sceVideoOut` + the first GNM packet handlers.
