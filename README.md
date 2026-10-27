# osfosKernel

osfosKernel is a small freestanding x86_64 operating system, written mainly in C and C++ with assembly and Rust components. It includes a shell, basic drivers, graphics, and sample programs.

## Build

Requirements: Clang/Clang++, LLD, NASM, GNU Make, Python 3, and GRUB rescue-image utilities (`grub-mkrescue` and its dependencies).

```sh
make
```

This creates `os.bin`, a bootable `os.iso`, and a 64 MiB `disk.img`. The disk image builds without game data. To include Doom I or Doom II, place legally obtained `doom1.wad` and/or `doom2.wad` files in the repository root and rebuild the image with `rm -f disk.img && make disk.img`. Launch them with `doom1` and `doom2` from the shell. Doom WAD files are ignored by Git and are not distributed here. Run `make clean` to remove build outputs.

## Source layout

- `arch/` — x86_64 boot and architecture code
- `include/` — kernel headers
- `kernel/` — kernel subsystems and programs, including the Doom port
- `rust/` — Rust components
- `tools/` — build and filesystem utilities

## Contributing

Keep changes focused and follow nearby code conventions. Kernel code is freestanding; do not assume hosted operating-system services or a standard library. Update the Makefile when adding files that its source discovery does not include. Build with `make` before submitting and describe the change.

## AI contributors

Read this file, the relevant source, and the Makefile before editing. Make the smallest change that completes the task, preserve unrelated behavior, and report what changed and what you verified.

## Third-party code and assets

The Doom source under `kernel/programs/doomgeneric/` retains its upstream license notices. Check the applicable license before redistributing changes or binaries. Game WAD files are proprietary data and must be obtained separately.
