#!/usr/bin/env python3
"""Build and compare Clang and GCC kernels using the startup QEMU benchmarks."""

import argparse
import csv
import datetime
import pathlib
import re
import shutil
import statistics
import subprocess
import sys

BENCHMARK = re.compile(r"^\[B\] ([a-zA-Z0-9_]+) rate=(\d+)/s elapsed_ms=(\d+)$")
VARIANTS = ("clang", "gcc")


def run_logged(command, log_path, cwd=None, timeout=None):
    with log_path.open("w") as log:
        try:
            return subprocess.run(command, cwd=cwd, stdout=log, stderr=subprocess.STDOUT,
                                  timeout=timeout).returncode
        except subprocess.TimeoutExpired:
            return None


def build(root, output, compiler, jobs):
    variant_dir = output / compiler
    variant_dir.mkdir()
    command = ["make", f"-j{jobs}", f"BUILD_DIR={variant_dir / 'build'}",
               f"OS_ISO={variant_dir / 'os.iso'}",
               f"DISK_IMAGE={variant_dir / 'disk.img'}"]
    if compiler == "gcc":
        command += [
            "COMPILER=gcc",
            "CC=gcc",
            "CC++=g++",
            "LESS_EXTR_FLAGS=-ftree-loop-vectorize -ftree-vectorize -fallow-store-data-races",
            "COMMON_FLAGS=-march=x86-64-v3 -ffast-math -ffreestanding -Wall -Wextra -w -m64 -mred-zone -fno-exceptions -fno-stack-protector -fno-rtti -fno-unwind-tables -fno-asynchronous-unwind-tables -fno-ident -fvisibility=hidden -flto -ffat-lto-objects -ffunction-sections -fdata-sections -fmerge-all-constants -Iinclude -Ikernel/programs/doomgeneric -Ithird_party/bearssl/inc -Ithird_party/bearssl/src",
            "LDFLAGS=-static -ffreestanding -nostdlib -flto -Wl,-O3 -Wl,-n -Wl,--gc-sections -Wl,-z,max-page-size=0x1000 -Wl,-T,arch/linker.ld"
        ]

    command.append("all")
    print(f"Building {compiler}...", flush=True)
    return run_logged(command, variant_dir / "build.log", cwd=root), variant_dir


def run_qemu(args, variant, run_number, iso, disk, output, logs):
    serial = output / variant / f"run-{run_number}.serial.log"
    qemu_log = output / variant / f"run-{run_number}.qemu.log"
    command = [args.qemu, "-machine", "pc,accel=tcg", "-cpu", "max",
               "-smp", "2", "-m", "512M", "-display", "none",
               "-monitor", "none", "-no-reboot", "-no-shutdown",
               "-serial", f"file:{serial}", "-cdrom", str(iso), "-drive",
               f"file={disk},format=raw,if=ide"]
    print(f"Running {variant}, pass {run_number}/{args.runs}...", flush=True)
    run_logged(command, qemu_log, timeout=args.boot_timeout)
    text = serial.read_text(errors="replace") if serial.exists() else ""
    complete = "[B] startup benchmark complete" in text
    if not complete:
        print(f"Warning: startup benchmark did not complete; see {serial}",
              file=sys.stderr)
    logs[(variant, run_number)] = (text, complete)


def write_results(output, logs, args):
    rows = []
    for (variant, run), (serial, complete) in logs.items():
        for line in serial.splitlines():
            match = BENCHMARK.match(line.strip())
            if match:
                name, rate, elapsed = match.groups()
                rows.append((variant, run, name, int(rate), int(elapsed), complete))
            else:
                skipped = re.match(r"^\[B\] ([a-zA-Z0-9_]+) skipped$", line.strip())
                if skipped:
                    rows.append((variant, run, skipped.group(1), "", "", complete))
    with (output / "results.csv").open("w", newline="") as file:
        writer = csv.writer(file)
        writer.writerow(("compiler", "run", "benchmark", "rate_per_second",
                         "elapsed_ms", "startup_complete"))
        writer.writerows(rows)

    rates = {}
    for variant, _, name, rate, _, complete in rows:
        if complete and rate != "":
            rates.setdefault((variant, name), []).append(rate)
    with (output / "summary.md").open("w") as summary:
        summary.write("# Clang vs GCC benchmark\n\n")
        summary.write(f"QEMU: `{args.qemu} -machine pc,accel=tcg -cpu max -smp 2 -m 512M`\n\n")
        summary.write(f"Requested runs per compiler: {args.runs}; only complete startup runs are summarized.\n\n")
        summary.write("| Benchmark | Clang median/s | GCC median/s | GCC change |\n|---|---:|---:|---:|\n")
        for name in sorted({name for _, name in rates}):
            clang = statistics.median(rates.get(("clang", name), [])) if rates.get(("clang", name)) else None
            gcc = statistics.median(rates.get(("gcc", name), [])) if rates.get(("gcc", name)) else None
            change = f"{(gcc / clang - 1) * 100:+.1f}%" if clang and gcc is not None else "n/a"
            summary.write(f"| {name} | {clang:,.0f} | {gcc:,.0f} | {change} |\n" if clang is not None and gcc is not None else
                          f"| {name} | {f'{clang:,.0f}' if clang is not None else 'n/a'} | {f'{gcc:,.0f}' if gcc is not None else 'n/a'} | {change} |\n")
        summary.write("\nRaw measurements are in `results.csv`; build logs and serial output are in each compiler directory.\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runs", type=int, default=4)
    parser.add_argument("--jobs", type=int, default=6)
    parser.add_argument("--boot-timeout", type=int, default=25)
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    parser.add_argument("--out", type=pathlib.Path)
    args = parser.parse_args()
    if min(args.runs, args.jobs, args.boot_timeout) < 1:
        parser.error("runs, jobs, and boot-timeout must be positive")
    root = pathlib.Path(__file__).resolve().parent.parent
    if not shutil.which(args.qemu):
        parser.error(f"QEMU executable not found: {args.qemu}")
    if not shutil.which("gcc") or not shutil.which("g++"):
        parser.error("GCC and G++ are required")
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    output = (args.out or root / "build" / f"compiler-compare-{stamp}").resolve()
    output.mkdir(parents=True, exist_ok=False)
    dirs = {}
    for compiler in VARIANTS:
        status, dirs[compiler] = build(root, output, compiler, args.jobs)
        if status != 0:
            print(f"{compiler} build failed; see {dirs[compiler] / 'build.log'}", file=sys.stderr)
            return status or 1
    logs = {}
    for run in range(1, args.runs + 1):
        order = VARIANTS if run % 2 else tuple(reversed(VARIANTS))
        for compiler in order:
            run_qemu(args, compiler, run, dirs[compiler] / "os.iso",
                     dirs["clang"] / "disk.img", output, logs)
    write_results(output, logs, args)
    print(f"Comparison complete: {output / 'summary.md'}")
    print(f"Raw results: {output / 'results.csv'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
