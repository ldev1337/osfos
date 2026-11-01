NASM = nasm
CC   = clang
CC++ = clang++

C_STD    = -std=c17
CPP_STD = -std=c++20

INCLUDES = -Iinclude -Ikernel/programs/doomgeneric

COMMON_FLAGS = -march=x86-64-v3 -mvzeroupper -mprefer-vector-width=256 \
	-ffreestanding -Wall -Wextra -w -mno-red-zone -m64 \
	-fno-exceptions -fno-stack-protector -fno-rtti \
	-fno-unwind-tables -fno-asynchronous-unwind-tables \
	-flto=full -ffunction-sections -fdata-sections -fmerge-all-constants $(INCLUDES)

FLAG_O2  = -O2
FLAG_O3    = -O3
FLAG_FM    = -Oz -ffast-math
FLAG_Oz    = -Oz

CFLAGS_C        = $(COMMON_FLAGS) $(FLAG_O2) $(C_STD)
CFLAGS_CPP      = $(COMMON_FLAGS) $(FLAG_O2) $(CPP_STD)
CFLAGS_C_PERF   = $(COMMON_FLAGS) $(FLAG_O3) $(C_STD)
CFLAGS_CPP_PERF = $(COMMON_FLAGS) $(FLAG_O3) $(CPP_STD)
CFLAGS_C_SIZE   = $(COMMON_FLAGS) $(FLAG_Oz) $(C_STD)
CFLAGS_CPP_SIZE = $(COMMON_FLAGS) $(FLAG_Oz) $(CPP_STD)
CFLAGS_DOOM_SIZE = $(COMMON_FLAGS) $(FLAG_FM) $(C_STD) \
	-fno-ident -fno-unwind-tables -fno-asynchronous-unwind-tables

LDFLAGS = -static -ffreestanding -nostdlib -flto=full -fuse-ld=lld \
	-Wl,-O3 -Wl,-n -Wl \
	-Wl,--gc-sections -Wl,--icf=all \
	-Wl,--strip-all -Wl,--build-id=none \
	-Wl,-T,arch/linker.ld

ASM_SRCS = $(shell find arch kernel -name '*.asm' $(EXCLUDE))
C_SRCS   = $(shell find arch kernel -name '*.c' $(EXCLUDE))
C++_SRCS = $(shell find arch kernel -name '*.cpp' $(EXCLUDE))

PROGRAM_OBJS      = $(patsubst %.cpp, %.o, $(wildcard kernel/programs/*.cpp))
PROGRAM_CPP_OBJS = $(patsubst %.cpp, %.o, $(shell find kernel/programs -name '*.cpp'))
PROGRAM_C_OBJS    = $(patsubst %.c, %.o, $(wildcard kernel/programs/doomgeneric/*.c))
GRAPHICS_OBJS      = $(patsubst %.cpp, %.o, $(wildcard kernel/graphics/*.cpp))
ARCH_PERF_C_OBJS  = arch/x86_64/memory.o arch/x86_64/string.o
PERF_CPP_OBJS      = kernel/lib/math.o kernel/fs/vfs.o $(GRAPHICS_OBJS)
SIZE_CPP_OBJS      = kernel/lib/compatlibc.o

OBJS = $(sort \
	$(ASM_SRCS:.asm=.o) \
	$(C_SRCS:.c=.o) \
	$(C++_SRCS:.cpp=.o) \
	$(PROGRAM_OBJS) \
	$(PROGRAM_CPP_OBJS) \
	$(PROGRAM_C_OBJS) \
	$(GRAPHICS_OBJS) \
	$(ARCH_PERF_C_OBJS) \
)

$(PERF_CPP_OBJS):    CFLAGS_CPP = $(CFLAGS_CPP_PERF)
$(ARCH_PERF_C_OBJS): CFLAGS_C   = $(CFLAGS_C_PERF)
$(SIZE_CPP_OBJS):    CFLAGS_CPP = $(CFLAGS_CPP_SIZE)
$(PROGRAM_OBJS) $(PROGRAM_CPP_OBJS): CFLAGS_CPP = $(CFLAGS_CPP_SIZE)
$(PROGRAM_C_OBJS):   CFLAGS_C   = $(CFLAGS_DOOM_SIZE)

opt_flag = $(lastword $(filter -O%,$(1)))
opt_tag  = $(patsubst -%,%,$(call opt_flag,$(1)))

TAG_C   = [CC  $(call opt_tag,$(CFLAGS_C))]
TAG_CPP = [C++ $(call opt_tag,$(CFLAGS_CPP))]

ifeq ($(V),1)
	Q =
else
	Q = @
endif

TERM_COLS ?= $(shell tput cols 2>/dev/null || echo 80)
FILE_WIDTH = $(shell awk -v cols=$(TERM_COLS) 'BEGIN { w = cols - 23; if (w < 23) w = 23; print w }')

define PRINT_TUI
	@flock /tmp/make_tui.lock sh -c ' \
	SZ=$$(wc -c < $@ | tr -d " "); \
	if [ $$SZ -ge 1048576 ]; then \
		SZ_FMT=$$(awk "BEGIN {printf \"%.2f MB\", $$SZ / 1048576}"); \
	elif [ $$SZ -ge 1024 ]; then \
		SZ_FMT=$$(awk "BEGIN {printf \"%.2f KB\", $$SZ / 1024}"); \
	else \
		SZ_FMT="$${SZ} B"; \
	fi; \
	TAG_RAW="$(1)"; \
	TAG_LEN=$${#TAG_RAW}; \
	PAD_LEN=$$(( 9 - TAG_LEN )); \
	[ $$PAD_LEN -lt 0 ] && PAD_LEN=0; \
	PADDING=$$(printf "%*s" $$PAD_LEN ""); \
	FILE_WIDTH=$(FILE_WIDTH); \
	printf "\033[1;36m%s\033[0m%s\033[37m%-*s\033[0m \033[1;90m│\033[0m \033[1;32m%10s\033[0m\n" "$$TAG_RAW" "$$PADDING" "$$FILE_WIDTH" "$(2)" "$$SZ_FMT" \
	'
endef

.PHONY: all clean simd_inspect

all: os.iso disk.img

DOOM1_WAD = $(firstword $(wildcard doom1.wad doom.wad))
DOOM2_WAD = $(wildcard doom2.wad)
DOOM_WADS = $(DOOM1_WAD) $(DOOM2_WAD)
ifneq ($(DOOM_WADS),)
disk.img: $(DOOM_WADS) tools/mkosfosfs.py
	@python3 tools/mkosfosfs.py $@ $(if $(DOOM1_WAD),--add /bin/doom/doom1.wad=$(DOOM1_WAD)) $(if $(DOOM2_WAD),--add /bin/doom/doom2.wad=$(DOOM2_WAD))
else
disk.img: tools/mkosfosfs.py
	@python3 tools/mkosfosfs.py $@
endif

%.o: %.asm
	@$(NASM) -Ox -f elf64 $< -o $@
ifeq ($(V),1)
else
	@$(call PRINT_TUI, [ASM   ], $<)
endif

%.o: %.c
	@$(CC) $(CFLAGS_C) -c $< -o $@
ifeq ($(V),1)
else
	@$(call PRINT_TUI, $(TAG_C), $<)
endif

%.o: %.cpp
	@$(CC++) $(CFLAGS_CPP) -c $< -o $@
ifeq ($(V),1)
else
	@$(call PRINT_TUI, $(TAG_CPP), $<)
endif

os.bin: $(OBJS)
	$(Q)$(CC) $(LDFLAGS) $(OBJS) -o $@
	$(Q)llvm-strip --strip-all --remove-section=.comment --remove-section=.note $@ 2>/dev/null || true
ifeq ($(V),1)
else
	$(Q)$(call PRINT_TUI, [LD    ], $@)
endif

os.iso: os.bin grub.cfg
	$(Q)mkdir -p iso/boot/grub
	$(Q)cp os.bin iso/boot/os.bin
	$(Q)cp grub.cfg iso/boot/grub/grub.cfg
	$(Q)grub-mkrescue -o os.iso iso >/dev/null 2>&1
	$(Q)rm -rf iso
ifeq ($(V),1)
else
	$(Q)$(call PRINT_TUI, [ISO   ], $@)
endif

simd: os.bin
	@total=$$(objdump -d os.bin 2>/dev/null | grep -E '\b(xmm|ymm|zmm)[0-9]+' | wc -l); \
	printf "\033[32mTotal SIMD instructions generated:\033[0m \033[1;33m%5d\033[0m\n\n" "$$total"; \
	objdump -d os.bin 2>/dev/null | grep -E '\b(xmm|ymm|zmm)[0-9]+' | awk -F'\t' '{print $$3}' | awk '{print $$1}' | sort | uniq -c | sort -nr | head -n 50 | awk '{printf "  \033[33m%-15s\033[0m : %5d times\n", $$2, $$1}'

clean:
	$(Q)rm -rf $(OBJS) os.bin os.iso iso kernel.profdata kernel.afdo perf.data
	$(Q)cargo clean --manifest-path rust/kernel_heap/Cargo.toml 2>/dev/null || true
ifeq ($(V),1)
else
	@printf "\033[1;31m  [CLEAN] \033[0m\033[37mAll build artifacts purged successfully.\033[0m\n"
endif
