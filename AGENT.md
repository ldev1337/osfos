# AGENTS.md

## Core Rules
- Prioritize performance, memory efficiency, and minimal binary size.
- Keep implementations simple, direct, and low-overhead.
- No unnecessary abstractions, wrappers, or dependencies.
- Preserve existing architecture and coding conventions.
- Never change public APIs unless explicitly requested.
- Preserve existing functionality.
- No placeholders, TODO implementations, or fake success paths.
- Never silently ignore errors.

## Workflow
1. Inspect relevant source files before editing.
2. Trace dependencies and callers before changing interfaces.
3. Identify the actual root cause before modifying code.
4. Make the smallest complete change.
5. Compile after modifications.
6. Run relevant tests or QEMU validation when available.
7. Inspect compiler errors and fix regressions.
8. Report exactly what changed and what was verified.

## Token Efficiency
- Read only files relevant to the task.
- Never dump entire directories or unrelated source files.
- Avoid repeatedly reading unchanged files.
- Prefer targeted grep, sed, and symbol searches.
- Do not repeat repository information already established.
- Do not explain obvious code.
- Keep responses short and technical.
- Do not generate documentation unless requested.
- Do not provide multiple alternatives unless necessary.
- Never repeatedly ask permission to continue an already authorized task.
- Continue working until the requested task is complete.

## C/C++ Rules
- Follow the existing C/C++ standard and project conventions.
- Avoid unnecessary STL containers and dynamic allocations.
- Prefer predictable memory layouts and cache-friendly structures.
- Minimize heap allocations, copying, and redundant initialization.
- Avoid unnecessary virtual dispatch, exceptions, and RTTI.
- Inline only when justified by performance.
- Avoid unnecessary null checks when invariants guarantee validity.
- Preserve existing symbol names and interfaces.

## Kernel Rules
- Freestanding environment: no assumptions about hosted OS facilities.
- Avoid libc dependencies unless already supported.
- Respect interrupt, locking, and memory-management invariants.
- Never allocate memory in interrupt context unless explicitly supported.
- Validate DMA alignment, physical addresses, and buffer lifetimes.
- Keep kernel paths efficient.
- Avoid unnecessary context switches and memory copies.
- Preserve bootloader and Multiboot compatibility.

## Performance
- Never claim an optimization without evidence.
- Benchmark before and after when practical.
- Measure CPU time, memory consumption, and binary size where relevant.
- Prefer measurable improvements over theoretical improvements.
- Avoid performance regressions disguised as cleaner code.
- Consider cache locality, branch prediction, and allocation overhead.

## Build & Debug
- Use the existing Makefile and toolchain.
- Do not change compiler flags without explicit justification.
- Never hide warnings or errors to make builds pass.
- Use the debugging tool at kernel/debug/

## Git
- Never reset, revert, or overwrite unrelated user changes.
- Do not create commits unless requested.
- Keep modifications scoped to the task.

## Final Response
Report only:
1. Changes made.
2. Files modified.
3. Build/test results.
4. Remaining issues, if any.

Be concise. Prioritize implementation over narration.