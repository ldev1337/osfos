# osfosKernel

osfosKernel is a small freestanding x86_64 operating system, written mainly in C and C++ with assembly and Rust components. It includes a shell, basic drivers, graphics, and sample programs.

## Build

Requirements: Clang/Clang++, LLD, NASM, GNU Make, Rust/Cargo with the `x86_64-unknown-none` target, Python 3, and GRUB rescue-image utilities (`grub-mkrescue` and its dependencies).

Install the bare-metal Rust target with `rustup target add x86_64-unknown-none`. The Rust network bridge uses the vendored smoltcp v0.14.0 source under `rust/smoltcp/`.

```sh
make
```

This creates a bootable `disk.img` with the kernel at `/boot/osfos` and the automounted LazioFS as a partition on the same disk. Boot it directly with QEMU:

```sh
qemu-system-x86_64 -enable-kvm -cpu host -smp 4 -m 64M -machine pc \
  -drive file=disk.img,format=raw,if=ide -boot c \
  -netdev user,id=net0 -device rtl8139,netdev=net0 \
  -serial stdio -no-reboot
```

Object files and the linked kernel binary (`build/os.bin`) stay under `build/`. To include Doom I or Doom II, place legally obtained `doom1.wad` and/or `doom2.wad` files in the repository root and rebuild with `make`. Launch them with `doom1` and `doom2` from the shell. Doom WAD files are ignored by Git and are not distributed here. Run `make clean` to remove build outputs.

Launch `pacman` from the shell to play Pac-Man. Use the arrow keys or WASD to move, and Escape to close the game window. The port renders directly into the OS window manager and does not use Sokol; sound is disabled because the kernel has no application audio output yet.

Network commands: `netup` acquires IPv4 settings by DHCP, `netinfo` prints the interface configuration, `ping <host>` sends ICMP echo requests, `tcp-echo <host> <port> <text>` tests TCP, and `fetch <url>` prints a bounded HTTP or HTTPS response. HTTPS uses BearSSL v0.6 with hostname and certificate-date validation, CMOS RTC time, and RDRAND entropy. The initial trust store contains only GTS Root R1 and ISRG Root X1, so HTTPS hosts must chain to one of those roots. If the RTC or RDRAND is unavailable, HTTPS fails closed. The network stack is polled by these commands; it is not yet a background service.

## Source layout

- `arch/` — x86_64 boot and architecture code
- `include/` — kernel headers
- `kernel/` — kernel subsystems and programs, including the Doom port
- `rust/` — Rust components
- `third_party/` — vendored smoltcp and BearSSL libraries with upstream license and version metadata
- `third_party/pacman/LICENSE.txt` — MIT license for the floooh/pacman.c game source
- `tools/` — build and filesystem utilities

## Contributing

Keep changes focused and follow nearby code conventions. Kernel code is freestanding; do not assume hosted operating-system services or a standard library. Update the Makefile when adding files that its source discovery does not include. Build with `make` before submitting and describe the change.

## AI contributors

Read this file, the relevant source, and the Makefile before editing. Make the smallest change that completes the task, preserve unrelated behavior, and report what changed and what you verified.

## Third-party code and assets

The Doom source under `kernel/programs/doomgeneric/` retains its upstream license notices. BearSSL is MIT-licensed; see `third_party/bearssl/LICENSE.txt`. Trust-anchor certificate sources and provenance are documented in `third_party/bearssl/CA-ROOTS.txt`. Check the applicable licenses before redistributing changes or binaries. Game WAD files are proprietary data and must be obtained separately.
