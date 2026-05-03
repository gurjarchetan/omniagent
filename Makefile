# omniagent/Makefile
#
# Targets:
#   make          — optimised dynamic build  (links shared libzstd, libpthread)
#   make static   — fully static binary      (requires libzstd.a, libpthread.a)
#   make debug    — debug build with ASan + UBSan
#   make clean    — remove all build artifacts
#   make install  — install binary to PREFIX/bin (default /usr/local/bin)
#
# Dependencies:
#   - GCC >= 7 or Clang >= 6  (C11 + _Atomic support)
#   - libzstd-dev (dynamic build)  OR  libzstd.a (static build)
#
# Quick start:
#   sudo apt-get install -y build-essential libzstd-dev
#   make -j$(nproc)
#   sudo ./omniagent -e otel-collector.internal -p 4318 -d

# ── Toolchain ────────────────────────────────────────────────────────────────
CC      ?= gcc
PREFIX  ?= /usr/local

# ── Directories ──────────────────────────────────────────────────────────────
SRC_DIR  = src
INC_DIR  = include
BLD_DIR  = build

# ── Source files ─────────────────────────────────────────────────────────────
SRCS := \
    $(SRC_DIR)/main.c             \
    $(SRC_DIR)/ring_buffer.c      \
    $(SRC_DIR)/pool.c             \
    $(SRC_DIR)/telemetry.c        \
    $(SRC_DIR)/pipeline.c         \
    $(SRC_DIR)/procfs_receiver.c  \
    $(SRC_DIR)/docker_receiver.c  \
    $(SRC_DIR)/inotify_receiver.c \
    $(SRC_DIR)/otlp_http_receiver.c \
    $(SRC_DIR)/memory_limiter.c   \
    $(SRC_DIR)/batch_processor.c  \
    $(SRC_DIR)/prometheus_exporter.c

OBJS := $(patsubst $(SRC_DIR)/%.c, $(BLD_DIR)/%.o, $(SRCS))

TARGET  = omniagent
TARGET_STATIC = omniagent-static

# ── Flags ────────────────────────────────────────────────────────────────────

# Core flags (shared by all builds)
CFLAGS_COMMON := \
    -std=c11                \
    -D_GNU_SOURCE           \
    -I$(INC_DIR)            \
    -Wall                   \
    -Wextra                 \
    -Wno-unused-parameter   \
    -Wmissing-prototypes    \
    -Wstrict-prototypes     \
    -Wshadow                \
    -Wformat=2              \
    -fstack-protector-strong \
    -D_FORTIFY_SOURCE=2

# Optimised build flags
CFLAGS_OPT := \
    $(CFLAGS_COMMON)        \
    -O3                     \
    -march=native           \
    -mtune=native           \
    -fomit-frame-pointer    \
    -flto                   \
    -DNDEBUG

# Debug build flags (ASan + UBSan + full debug info)
CFLAGS_DBG := \
    $(CFLAGS_COMMON)        \
    -O0                     \
    -g3                     \
    -fno-omit-frame-pointer \
    -fsanitize=address,undefined,leak \
    -fno-sanitize-recover=all

# Default CFLAGS
CFLAGS ?= $(CFLAGS_OPT)

# Linker flags — dynamic build
LDFLAGS_DYN := \
    -pthread                \
    -lm                     \
    -flto

# Linker flags — static build
# Assumes libzstd.a and libpthread.a are available in the standard library path
# or set STATIC_LIBDIR to point at a custom sysroot.
STATIC_LIBDIR ?=
LDFLAGS_STATIC := \
    -static                             \
    $(if $(STATIC_LIBDIR),-L$(STATIC_LIBDIR)) \
    -lpthread                           \
    -lm                                 \
    -flto

# ── Build rules ──────────────────────────────────────────────────────────────

.PHONY: all static debug clean install strip size help

all: $(BLD_DIR) $(TARGET)

# ── Default (optimised, dynamic) target ──────────────────────────────────────
$(TARGET): CFLAGS := $(CFLAGS_OPT)
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS_DYN)
	@echo ""
	@echo "  Built: $@ (dynamic)"
	@$(MAKE) --no-print-directory _size TARGET=$@

# ── Static target ─────────────────────────────────────────────────────────────
static: CFLAGS := $(CFLAGS_OPT)
static: $(BLD_DIR) $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET_STATIC) $(OBJS) $(LDFLAGS_STATIC)
	@echo ""
	@echo "  Built: $(TARGET_STATIC) (static)"
	@$(MAKE) --no-print-directory _size TARGET=$(TARGET_STATIC)

# ── Debug target ─────────────────────────────────────────────────────────────
debug: CFLAGS := $(CFLAGS_DBG)
debug: $(BLD_DIR)
	$(CC) $(CFLAGS_DBG) -o $(TARGET)-debug $(SRCS) -pthread -lzstd -lm
	@echo "  Built: $(TARGET)-debug (with ASan + UBSan)"

# ── Object files ─────────────────────────────────────────────────────────────
$(BLD_DIR)/%.o: $(SRC_DIR)/%.c | $(BLD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BLD_DIR):
	mkdir -p $(BLD_DIR)

# ── Strip ─────────────────────────────────────────────────────────────────────
strip: $(TARGET)
	strip --strip-all $(TARGET)
	@$(MAKE) --no-print-directory _size TARGET=$(TARGET)

# ── Size report ──────────────────────────────────────────────────────────────
_size:
	@if command -v size >/dev/null 2>&1; then \
	    echo "  Section sizes:"; \
	    size $(TARGET); \
	fi
	@echo "  Binary size: $$(du -sh $(TARGET) | cut -f1)"

size: $(TARGET)
	@$(MAKE) --no-print-directory _size TARGET=$(TARGET)

# ── Install ───────────────────────────────────────────────────────────────────
install: $(TARGET)
	install -D -m 0755 $(TARGET) $(PREFIX)/bin/$(TARGET)
	@echo "  Installed: $(PREFIX)/bin/$(TARGET)"

# ── Clean ─────────────────────────────────────────────────────────────────────
clean:
	rm -rf $(BLD_DIR) $(TARGET) $(TARGET_STATIC) $(TARGET)-debug
	@echo "  Cleaned"

# ── Help ─────────────────────────────────────────────────────────────────────
help:
	@echo ""
	@echo "  OmniAgent build targets:"
	@echo ""
	@echo "    make              — optimised dynamic binary (default)"
	@echo "    make static       — fully static binary"
	@echo "    make debug        — debug binary with ASan + UBSan"
	@echo "    make strip        — strip symbols from binary"
	@echo "    make install      — install to PREFIX/bin (default /usr/local/bin)"
	@echo "    make clean        — remove build artifacts"
	@echo "    make size         — show binary and section sizes"
	@echo ""
	@echo "  Variables:"
	@echo "    CC=<compiler>       Override compiler (default: gcc)"
	@echo "    PREFIX=<path>       Install prefix    (default: /usr/local)"
	@echo "    STATIC_LIBDIR=<dir> Directory for static libs"
	@echo ""
	@echo "  Prerequisites:"
	@echo "    sudo apt-get install -y build-essential libzstd-dev"
	@echo ""
	@echo "  Run:"
	@echo "    sudo ./omniagent -e otel-collector:4317 -t my-api-key -d"
	@echo ""
