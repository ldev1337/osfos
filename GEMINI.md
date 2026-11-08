# osfosKernel — Token Budget + Project Map

Freestanding x86_64 OS (C/C++20 + NASM/GAS + Rust smoltcp + vendored BearSSL). No hosted libc/OS services. Headers live under `include/`; impl under `kernel/`, `arch/`. Build: `make` → `os.iso` + `disk.img` (objs in `build/`). FS image via `tools/mklaziofs.py`.

Treat every tool call / file read / reply as expensive. Prefer finishing with the fewest tokens.

## Do less
- Act now. No plans, phases, subagents, or "Proceed" checkpoints unless asked.
- Do **not** explore to orient. Use the map below; open only files for the current ask.
- Never re-read known context. No proactive git/status/diff/log/lint/full-build.
- Prefer path-known `Read` / tight `Grep` over `Glob` / tree walks. Use offset/limit.
- Batch independent tool calls. No serial check loops.
- Do not read README, AGENT.md, third_party trees, doomgeneric, or BearSSL sources unless the task is specifically about them.
- Skip `build/`, `disk.img`, `os.iso`, WADs, `compile_commands.json`.

## Edit tight
- Smallest correct change. Match nearby style. No drive-by refactors/docs/comments.
- Freestanding: no stdlib/OS assumptions; no new deps without justification.
- New sources usually auto-picked by Makefile `find`; update Makefile only if discovery/EXCLUDE/flags need it.
- Programs: add `kernel/programs/<name>.cpp` (+ header APIs if needed) and wire launch in `kernel/shell/shell.cpp`.
- If blocked, one short question — no speculative reads.

## Reply tight
- Lead with the result. No preamble, narration, or task restatement.
- Cite sparingly. No unsolicited summaries/test plans/follow-ups.

## Hard bans
- Architect→implementer→reviewer / multi-agent workflows
- Orientation sweeps of the repo
- Unsolicited markdown/docs/todos

---

## Layout (do not rediscover)
| Path | Role |
|------|------|
| `arch/x86_64/` | Boot, GDT, interrupts, paging, multiboot, string/memory |
| `arch/linker.ld` | Link script |
| `kernel/main.cpp` | Boot init order → WM + shell threads |
| `kernel/scheduler.cpp` | Threads / scheduler |
| `kernel/shell/shell.cpp` | Shell UI + **all command dispatch** |
| `kernel/drivers/` | ata, e1000, fb, kbd, mouse, pci, pit, usb/xhci |
| `kernel/fs/laziofs.cpp` | Custom FS |
| `kernel/net/` | C++ bridge + HTTP(S) client + trust anchors |
| `kernel/graphics/` | vfb, fonts, window manager |
| `kernel/memory/` | PMM + heap |
| `kernel/lib/` | math, random, cpp runtime, app/ui helpers |
| `kernel/programs/` | User apps/games (`*.cpp`); `doomgeneric/`; `shitoffice/` |
| `rust/net/` | smoltcp Rust crate → `libosfos_net.a` |
| `include/kernel/**` | Public headers (mirror subsystem) |
| `include/arch/x86_64/` | Arch headers |
| `third_party/bearssl/` | Vendored TLS (touch only for TLS build/port issues) |
| `tools/mklaziofs.py` | disk.img builder |
| `grub.cfg` | Boot menu |
| `Makefile` | Clang/LLD freestanding build, LTO, ISO |

Namespaces: `Kernel::Shell`, `Kernel::Drivers`, `Kernel::Net`, `Kernel::Graphics`, `Kernel::Memory`, `Kernel::Apps`, `Kernel::Games`.

---

## Workflow → open these first (stop after enough context)
| Task | Start here |
|------|------------|
| Boot / early init | `kernel/main.cpp` → `arch/x86_64/boot.asm`, `multiboot.cpp`, `include/kernel/bootinfo.h` |
| Shell command / CLI | `kernel/shell/shell.cpp` (dispatch ~help/net/apps); `include/kernel/shell/shell.h` |
| New app/game | `kernel/programs/<app>.cpp` + register in `shell.cpp`; UI helpers `include/kernel/lib/{appcommon,uicommon}.h` |
| Graphics / windows | `kernel/graphics/window_manager.cpp`, `vfb.cpp`; `drivers/framebuffer.cpp` |
| Keyboard / mouse / timer | `kernel/drivers/{keyboard,mouse,pit}.cpp` + matching `include/kernel/drivers/*.h` |
| Disk / FS | `kernel/drivers/ata.cpp` → `kernel/fs/laziofs.cpp` → `include/kernel/fs/laziofs.h` → `tools/mklaziofs.py` |
| NIC / Ethernet | `kernel/drivers/e1000.cpp` + `include/kernel/drivers/e1000.h`; PCI `drivers/pci.cpp` |
| TCP/IP / DHCP / ping | `rust/net/src/lib.rs` ↔ `kernel/net/smoltcp_bridge.cpp` + `include/kernel/net/smoltcp_bridge.h` |
| HTTP / HTTPS / fetch | `kernel/net/http_client.cpp`; anchors `kernel/net/bearssl_trust_anchors.c` |
| Memory alloc | `kernel/memory/{pmm,heap}` + `include/kernel/memory/*`; arch `arch/x86_64/memory.c` |
| Scheduler / threads | `kernel/scheduler.cpp`, `include/kernel/scheduler.h` |
| Doom | `kernel/programs/doom.cpp` + `doomgeneric/`; WADs via `make disk.img` |
| Office-ish apps | `kernel/programs/shitcell.cpp`, `shitoffice/shitpad.cpp`; launched from shell |
| Build / link / ISO | `Makefile`, `arch/linker.ld`, `grub.cfg` |
| USB | `kernel/drivers/usb/` + `include/kernel/drivers/usb/` |

Net shell cmds: `netup`, `netinfo`, `ping`, `tcp-echo`, `fetch` — implemented via shell → smoltcp bridge / http_client. Stack is command-polled, not a background service.

## Constraints
- Correctness > token savings. Never skip a read that would cause a wrong edit.
- Prefer editing the subsystem file from the table; grep only if the map misses.
- Do not rewrite vendored Doom/BearSSL/smoltcp unless required for the ask.
