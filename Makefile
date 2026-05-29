# ithaca-legacy — top-level Makefile
# -----------------------------------
# Build orchestrator: auto-detekce platformy / generatoru / jader.
# Cile: help (default) / check-tools / fetch-third-party / configure /
#       build / rebuild / test / smoke / clean / info
#
# Override-able promenne: BUILD_DIR, BUILD_TYPE, GENERATOR, JOBS
# Priklady:
#   make check-tools && make fetch-third-party && make build && make smoke
#   make BUILD_TYPE=Debug build
#   make GENERATOR=Ninja configure

.DEFAULT_GOAL := help
.DELETE_ON_ERROR:

# -- Platform detection --
ifeq ($(OS),Windows_NT)
    PLATFORM := Windows
    EXE      := .exe
    NULL     := NUL
    RM_RF    := cmake -E rm -rf
    MKDIR_P  := cmake -E make_directory
else
    UNAME_S := $(shell uname -s 2>/dev/null)
    ifeq ($(UNAME_S),Darwin)
        PLATFORM := macOS
    else ifeq ($(UNAME_S),Linux)
        PLATFORM := Linux
    else
        PLATFORM := Unknown
    endif
    EXE      :=
    NULL     := /dev/null
    RM_RF    := rm -rf
    MKDIR_P  := mkdir -p
endif

BUILD_DIR  ?= build
BUILD_TYPE ?= Release

ifeq ($(PLATFORM),macOS)
    JOBS ?= $(shell sysctl -n hw.ncpu 2>/dev/null || echo 4)
else ifeq ($(PLATFORM),Linux)
    JOBS ?= $(shell nproc 2>/dev/null || echo 4)
else
    JOBS ?= 4
endif

# Generator: Ninja kdyz je v PATH, jinak Unix Makefiles (na non-Windows).
ifeq ($(GENERATOR),)
    HAS_NINJA := $(shell command -v ninja 2>$(NULL) >$(NULL) && echo yes)
    ifeq ($(HAS_NINJA),yes)
        GENERATOR := Ninja
    else
        GENERATOR := Unix Makefiles
    endif
endif

.PHONY: help
help:
	@printf "\nithaca-legacy — build orchestrator\n"
	@printf "  Platforma: $(PLATFORM)   Generator: $(GENERATOR)   Jobs: $(JOBS)\n\n"
	@printf "Cile:\n"
	@printf "  make check-tools        over cmake / ninja\n"
	@printf "  make fetch-third-party  stahni vendored deps\n"
	@printf "  make configure          CMake configure -> $(BUILD_DIR)/\n"
	@printf "  make build              zkompiluj vse\n"
	@printf "  make rebuild            clean + configure + build\n"
	@printf "  make test               spust doctest pres ctest\n"
	@printf "  make smoke              ithaca-cli --selftest\n"
	@printf "  make clean              smaze $(BUILD_DIR)/\n"
	@printf "  make info               vypis detekovane hodnoty\n\n"

.PHONY: info
info:
	@printf "PLATFORM=$(PLATFORM) GENERATOR=$(GENERATOR) BUILD_TYPE=$(BUILD_TYPE) JOBS=$(JOBS)\n"

.PHONY: check-tools
check-tools:
	@cmake --version >$(NULL) 2>&1 || (printf "cmake neni v PATH (brew install cmake)\n" && exit 1)
	@printf "cmake OK: "; cmake --version | head -1
	@command -v ninja >$(NULL) 2>&1 && printf "ninja OK\n" || printf "ninja chybi (fallback $(GENERATOR))\n"

.PHONY: fetch-third-party
fetch-third-party:
	@bash tools/fetch-third-party.sh

.PHONY: configure
configure:
	@cmake -S . -B $(BUILD_DIR) -G "$(GENERATOR)" -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)

$(BUILD_DIR)/CMakeCache.txt:
	@$(MAKE) --no-print-directory configure

.PHONY: build
build: $(BUILD_DIR)/CMakeCache.txt
	@cmake --build $(BUILD_DIR) --config $(BUILD_TYPE) --parallel $(JOBS)
	@printf "Build OK. Binarka: $(BUILD_DIR)/ithaca-cli$(EXE)\n"

.PHONY: rebuild
rebuild: clean configure build

.PHONY: test
test: build
	@ctest --test-dir $(BUILD_DIR) --output-on-failure

.PHONY: smoke
smoke: build
	@printf "Smoke test — ithaca-cli --selftest\n"
	@if [ -x $(BUILD_DIR)/ithaca-cli$(EXE) ]; then \
	    $(BUILD_DIR)/ithaca-cli$(EXE) --selftest; \
	else \
	    printf "ithaca-cli nenalezen v $(BUILD_DIR)/\n" && exit 1; \
	fi
	@printf "Smoke OK.\n"

.PHONY: clean
clean:
	@$(RM_RF) $(BUILD_DIR)
	@printf "Build dir smazan.\n"
