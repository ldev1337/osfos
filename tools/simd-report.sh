#!/usr/bin/env bash
set -Eeuo pipefail

ROOT="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"
cd "$ROOT"

OUT="${1:-build/simd-report-$(date +%Y%m%d-%H%M%S).txt}"
mkdir -p "$(dirname "$OUT")"

# Memory movement
MEMORY_INSTS=(
    vmovdqu vmovdqa vmovups vmovaps
    vmovd vmovq movdqu movdqa movups movaps
    mov movzx movsx movsxd
)

# Address generation and integer operations
ADDRESS_INSTS=(
    lea add sub inc dec
    shl shr sar
    imul and or xor
)

# SIMD floating-point
SIMD_FLOAT_INSTS=(
    vaddps vsubps vmulps vdivps
    vaddpd vsubpd vmulpd vdivpd
    vmaxps vminps vmaxpd vminpd
    vsqrtps vsqrtpd
    vfmadd132ps vfmadd213ps vfmadd231ps
)

# SIMD integer and logical operations
SIMD_INT_INSTS=(
    vpaddd vpsubd vpmulld
    vpaddq vpsubq
    vpand vpandn vpor vpxor
    vpcmpeqd vpcmpgtd
    vblendvps vpermilps vshufps
    vpshufd vpunpckldq vpunpckhdq
    vpslld vpsrld vpsrad
)

# Cache and memory ordering
CACHE_INSTS=(
    prefetcht0 prefetcht1 prefetcht2
    prefetchnta clflush clflushopt
    mfence lfence sfence
)

# Branches and conditional operations
BRANCH_INSTS=(
    jmp je jne jz jnz
    ja jae jb jbe
    jl jle jg jge
    cmovz cmovnz setz setnz
)

ALL_INSTS=(
    "${MEMORY_INSTS[@]}"
    "${ADDRESS_INSTS[@]}"
    "${SIMD_FLOAT_INSTS[@]}"
    "${SIMD_INT_INSTS[@]}"
    "${CACHE_INSTS[@]}"
    "${BRANCH_INSTS[@]}"
)

{
    echo "============================================================"
    echo "OSFOS INSTRUCTION AUDIT"
    echo "============================================================"
    echo "Date:       $(date -Is)"
    echo "Directory:  $ROOT"
    echo "Kernel:     $(uname -a)"
    echo "Compiler:"
    clang --version | head -n 2
    echo
    echo "Git commit: $(git rev-parse HEAD 2>/dev/null || echo unknown)"
    echo "Git branch: $(git branch --show-current 2>/dev/null || echo unknown)"
    echo
    echo "Instruction count: ${#ALL_INSTS[@]}"
    echo "Inspection: make filesimd INST=<instruction>"
    echo "============================================================"
    echo
} > "$OUT"

TOTAL=${#ALL_INSTS[@]}
CURRENT=0

for inst in "${ALL_INSTS[@]}"; do
    ((++CURRENT))

    {
        echo
        echo "============================================================"
        echo "[$CURRENT/$TOTAL] INSTRUCTION: $inst"
        echo "============================================================"
    } >> "$OUT"

    printf '[%3d/%3d] Inspecting %-20s\n' "$CURRENT" "$TOTAL" "$inst"

    if make --no-print-directory filesimd INST="$inst" >> "$OUT" 2>&1; then
        echo "[OK] $inst" >> "$OUT"
    else
        echo "[ERROR] Inspection failed for $inst" >> "$OUT"
    fi
done

{
    echo
    echo "============================================================"
    echo "AUDIT COMPLETE"
    echo "============================================================"
    echo "Instructions inspected: $TOTAL"
    echo "Report: $OUT"
} >> "$OUT"

sed -i "/^make\[1\]: 'build\/filesimd\/.*' is up to date\.$/d" build/simd-report-*.txt

printf '\nReport generated: %s\n' "$OUT"