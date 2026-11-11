#!/usr/bin/env python3
"""Compare this redzone working tree with the no-red-zone main branch."""

import argparse
import csv
import datetime
import pathlib
import re
import shutil
import statistics
import subprocess
import sys
import tempfile


BENCHMARK = re.compile(r"^\[B\] ([a-zA-Z0-9_]+) rate=(\d+)/s elapsed_ms=(\d+)$")
VARIANTS = ("redzone", "main-no-redzone")


def run_logged(command, log_path, cwd=None, timeout=None):
    with log_path.open("w") as log:
        try:
            return subprocess.run(
                command, cwd=cwd, stdout=log, stderr=subprocess.STDOUT,
                timeout=timeout,
            ).returncode
        except subprocess.TimeoutExpired:
            return None


def git_output(root, *args):
    return subprocess.check_output(["git", *args], cwd=root, text=True).strip()


def build_redzone(root, output, jobs):
    variant_dir = output / "redzone"
    variant_dir.mkdir()
    command = [
        "make", f"-j{jobs}", f"BUILD_DIR={variant_dir / 'build'}",
        f"OS_ISO={variant_dir / 'os.iso'}",
        f"DISK_IMAGE={variant_dir / 'disk.img'}",
        "RED_ZONE_FLAG=-mred-zone", "all",
    ]
    print("Building redzone from the current working tree...", flush=True)
    return run_logged(command, variant_dir / "build.log", cwd=root), variant_dir


def build_main(root, output, jobs, temp_root):
    variant_dir = output / "main-no-redzone"
    variant_dir.mkdir()
    source_dir = temp_root / "main-source"
    source_dir.mkdir()
    archive = temp_root / "main.tar"
    with archive.open("wb") as archive_file:
        subprocess.run(
            ["git", "archive", "--format=tar", "main"], cwd=root,
            stdout=archive_file, check=True,
        )
    subprocess.run(["tar", "-xf", str(archive), "-C", str(source_dir)], check=True)

    main_makefile = git_output(root, "show", "main:Makefile")
    if "-mno-red-zone" not in main_makefile:
        raise RuntimeError("main:Makefile does not contain -mno-red-zone")

    command = ["make", f"-j{jobs}"]
    print("Building main from the main branch archive...", flush=True)
    status = run_logged(command, variant_dir / "build.log", cwd=source_dir)
    if status != 0:
        return status, variant_dir, source_dir
    return status, variant_dir, source_dir


def run_qemu(args, variant, run_number, iso_path, common_disk, output, logs):
    variant_dir = output / variant
    serial_log = variant_dir / f"run-{run_number}.serial.log"
    qemu_log = variant_dir / f"run-{run_number}.qemu.log"
    command = [
        args.qemu, "-machine", "pc,accel=kvm", "-cpu", "max", "-smp", "4",
        "-m", "512M", "-display", "none", "-monitor", "none",
        "-no-reboot", "-no-shutdown", "-serial", f"file:{serial_log}",
        "-cdrom", str(iso_path), "-drive",
        f"file={common_disk},format=raw,if=ide",
    ]
    print(f"Running {variant}, pass {run_number}/{args.runs}...", flush=True)
    run_logged(command, qemu_log, timeout=args.boot_timeout)
    serial_text = serial_log.read_text(errors="replace") if serial_log.exists() else ""
    complete = "[B] startup benchmark complete" in serial_text
    if not complete:
        print(f"Warning: startup benchmark did not complete; see {serial_log}",
              file=sys.stderr)
    logs[(variant, run_number)] = (serial_text, complete)


def write_results(output, logs, args, redzone_ref, main_ref, dirty):
    rows = []
    for (variant, run_number), (serial_text, complete) in logs.items():
        for line in serial_text.splitlines():
            match = BENCHMARK.match(line.strip())
            if match:
                name, rate, elapsed = match.groups()
                rows.append((variant, run_number, name, int(rate), int(elapsed), complete))
            else:
                skipped = re.match(r"^\[B\] ([a-zA-Z0-9_]+) skipped$", line.strip())
                if skipped:
                    rows.append((variant, run_number, skipped.group(1), "", "", complete))

    with (output / "results.csv").open("w", newline="") as csv_file:
        writer = csv.writer(csv_file)
        writer.writerow(("variant", "run", "benchmark", "rate_per_second",
                         "elapsed_ms", "startup_complete"))
        writer.writerows(rows)

    rates = {}
    for variant, _, name, rate, _, complete in rows:
        if complete and rate != "":
            rates.setdefault((variant, name), []).append(rate)
    names = sorted({name for _, name in rates})
    with (output / "summary.md").open("w") as summary:
        summary.write("# Red-zone vs main benchmark\n\n")
        summary.write(f"Redzone source: current working tree at `{redzone_ref}`")
        summary.write(" (dirty)\n\n" if dirty else "\n\n")
        summary.write(f"No-red-zone baseline: `main` at `{main_ref}`\n\n")
        summary.write(f"QEMU: `{args.qemu} -machine pc,accel=kvm -cpu max -smp 4 -m 512M`\n\n")
        summary.write(f"Requested runs per variant: {args.runs}; summary uses complete startup runs only.\n\n")
        summary.write("| Benchmark | redzone median/s | main no-red-zone median/s | Redzone change |\n")
        summary.write("|---|---:|---:|---:|\n")
        for name in names:
            red = rates.get(("redzone", name), [])
            main = rates.get(("main-no-redzone", name), [])
            red_median = statistics.median(red) if red else None
            main_median = statistics.median(main) if main else None
            if red_median is not None and main_median:
                change = f"{(red_median / main_median - 1) * 100:+.1f}%"
            else:
                change = "n/a"
            red_text = f"{red_median:,.0f}" if red_median is not None else "n/a"
            main_text = f"{main_median:,.0f}" if main_median is not None else "n/a"
            summary.write(f"| {name} | {red_text} | {main_text} | {change} |\n")
        summary.write("\nRaw rows are in `results.csv`; full serial output and build logs are in each variant directory.\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runs", type=int, default=4,
                        help="QEMU runs per variant (default: 4)")
    parser.add_argument("--jobs", type=int, default=6,
                        help="parallel build jobs (default: 6)")
    parser.add_argument("--boot-timeout", type=int, default=25,
                        help="seconds per QEMU run (default: 25)")
    parser.add_argument("--qemu", default="qemu-system-x86_64",
                        help="QEMU executable")
    parser.add_argument("--out", type=pathlib.Path,
                        help="output directory (default: build/redzone-compare-TIMESTAMP)")
    args = parser.parse_args()
    if args.runs < 1 or args.jobs < 1 or args.boot_timeout < 1:
        parser.error("runs, jobs, and boot-timeout must be positive")

    root = pathlib.Path(__file__).resolve().parent.parent
    if not shutil.which(args.qemu):
        parser.error(f"QEMU executable not found: {args.qemu}")
    redzone_ref = git_output(root, "rev-parse", "--short", "HEAD")
    main_ref = git_output(root, "rev-parse", "--short", "main")
    dirty = bool(git_output(root, "status", "--porcelain"))
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    output = (args.out or root / "build" / f"redzone-compare-{stamp}").resolve()
    output.mkdir(parents=True, exist_ok=False)

    redzone_status, redzone_dir = build_redzone(root, output, args.jobs)
    if redzone_status != 0:
        print(f"Redzone build failed; see {redzone_dir / 'build.log'}", file=sys.stderr)
        return redzone_status or 1

    logs = {}
    with tempfile.TemporaryDirectory(prefix="osfos-main-") as temp_name:
        temp_root = pathlib.Path(temp_name)
        main_status, main_dir, main_source = build_main(root, output, args.jobs, temp_root)
        if main_status != 0:
            print(f"Main build failed; see {main_dir / 'build.log'}", file=sys.stderr)
            return main_status or 1

        # Both kernels use the disk image generated by main so VFS input is identical.
        common_disk = main_source / "disk.img"
        for run_number in range(1, args.runs + 1):
            order = VARIANTS if run_number % 2 else tuple(reversed(VARIANTS))
            for variant in order:
                iso = redzone_dir / "os.iso" if variant == "redzone" else main_source / "os.iso"
                run_qemu(args, variant, run_number, iso, common_disk, output, logs)

        write_results(output, logs, args, redzone_ref, main_ref, dirty)

    print(f"Comparison complete: {output / 'summary.md'}")
    print(f"Raw results: {output / 'results.csv'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
