# ithaca-legacy — top-level Makefile
# -----------------------------------
# Build orchestrator: auto-detekce platformy / generatoru / jader.
#
# Seznam vsech cilu:  make help   (default goal)
#
# Override-able promenne: BUILD_DIR, BUILD_TYPE, GENERATOR, JOBS, SECRET_FILE
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
    # Recepty pouzivaji bash semantiku (printf, command -v, [ -x ], mkdir -p);
    # vyzadujeme bash shell i na Windows (Git Bash / MSYS). V bashi je NUL bezny
    # nazev souboru — null device je /dev/null.
    SHELL    := bash
    NULL     := /dev/null
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
SECRET_FILE ?= secret/bank_secret.key

ifeq ($(PLATFORM),macOS)
    JOBS ?= $(shell sysctl -n hw.ncpu 2>/dev/null || echo 4)
else ifeq ($(PLATFORM),Linux)
    JOBS ?= $(shell nproc 2>/dev/null || echo 4)
else
    JOBS ?= 4
endif

# Generator: Ninja kdyz je v PATH; jinak Unix Makefiles (Unix) nebo
# Visual Studio 17 2022 (Windows — Unix Makefiles by hledalo gcc, ktere s MSVC
# toolchainem chybi). Bundlovana Ninja z VS Installer-u byva mimo PATH, takze
# default fallback je VS generator.
ifeq ($(GENERATOR),)
    HAS_NINJA := $(shell command -v ninja 2>$(NULL) >$(NULL) && echo yes)
    ifeq ($(HAS_NINJA),yes)
        GENERATOR := Ninja
    else ifeq ($(PLATFORM),Windows)
        GENERATOR := Visual Studio 17 2022
    else
        GENERATOR := Unix Makefiles
    endif
endif

# -- Napoveda --
# `help` se generuje automaticky z anotaci cilu: kazdy radek tvaru
#   <cil>: ## <popis>
# se objevi ve vypisu. Diky tomu seznam cilu nemuze "ujet" od reality.
# (Popisy jsou prosty text — make promenne se v nich neexpanduji.)
.PHONY: help
help: ## vypis tuto napovedu (seznam cilu)
	@printf "\nithaca-legacy — build orchestrator\n"
	@printf "  Platforma: $(PLATFORM)   Generator: $(GENERATOR)   Jobs: $(JOBS)\n\n"
	@printf "Cile (make <cil>):\n"
	@awk 'BEGIN {FS = ":.*## "} /^[a-zA-Z0-9_-]+:.*## / {printf "  %-18s %s\n", $$1, $$2}' $(MAKEFILE_LIST)
	@printf "\nOverride promenne: BUILD_DIR BUILD_TYPE GENERATOR JOBS SECRET_FILE\n"
	@printf "Priklad: make BUILD_TYPE=Debug build   |   make new-license SRC=<dyn-banka> DST=<cil>\n\n"

.PHONY: info
info: ## vypis detekovane hodnoty (platform/generator/build-type/jobs)
	@printf "PLATFORM=$(PLATFORM) GENERATOR=$(GENERATOR) BUILD_TYPE=$(BUILD_TYPE) JOBS=$(JOBS)\n"

.PHONY: check-tools
check-tools: ## over dostupnost cmake / ninja
	@cmake --version >$(NULL) 2>&1 || (printf "cmake neni v PATH (brew install cmake)\n" && exit 1)
	@printf "cmake OK: "; cmake --version | head -1
	@command -v ninja >$(NULL) 2>&1 && printf "ninja OK\n" || printf "ninja chybi (fallback $(GENERATOR))\n"

.PHONY: fetch-third-party
fetch-third-party: ## stahni vendored third-party zavislosti
	@bash tools/fetch-third-party.sh

.PHONY: bank-secret
bank-secret: ## vygeneruj master secret klic (SECRET_FILE)
	@python3 tools/gen-bank-secret.py $(SECRET_FILE)

.PHONY: configure
configure: ## CMake configure do BUILD_DIR
	@python3 tools/gen-bank-secret.py $(SECRET_FILE)
	@cmake -S . -B $(BUILD_DIR) -G "$(GENERATOR)" -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)

$(BUILD_DIR)/CMakeCache.txt:
	@$(MAKE) --no-print-directory configure

.PHONY: build
build: $(BUILD_DIR)/CMakeCache.txt ## zkompiluj vse (auto-configure pokud treba)
	@cmake --build $(BUILD_DIR) --config $(BUILD_TYPE) --parallel $(JOBS)
	@printf "Build OK. Binarka: $(BUILD_DIR)/ithaca-cli$(EXE)\n"

.PHONY: rebuild
rebuild: clean configure build ## clean + configure + build

.PHONY: test
test: build ## spust doctest pres ctest
	@ctest --test-dir $(BUILD_DIR) --output-on-failure

.PHONY: smoke
smoke: build ## smoke test — ithaca-cli batch render do test-samples/
	@printf "Smoke test — batch render do test-samples/smoke.wav\n"
	@mkdir -p test-samples/_smoke_bank test-samples
	@# Vyrob 1 fixture WAV (1s stereo 48k konst. amplituda) pres maly python helper.
	@python3 -c "import wave,struct; \
f=wave.open('test-samples/_smoke_bank/m060-vel4-f48.wav','wb'); \
f.setnchannels(2); f.setsampwidth(2); f.setframerate(48000); \
f.writeframes(b''.join(struct.pack('<hh',8000,8000) for _ in range(48000))); f.close()"
	@if [ -x $(BUILD_DIR)/ithaca-cli ]; then \
	    $(BUILD_DIR)/ithaca-cli --render test-samples/_smoke_bank --out test-samples/smoke.wav; \
	else \
	    printf "ithaca-cli nenalezen\n" && exit 1; \
	fi
	@rm -rf test-samples/_smoke_bank
	@printf "Smoke OK — test-samples/smoke.wav\n"

.PHONY: clean
clean: ## smaze BUILD_DIR
	@$(RM_RF) $(BUILD_DIR)
	@printf "Build dir smazan.\n"

# -- Licencovana (sifrovana) packed banka --
# Zabali dynamickou banku do SIFROVANE soundbank.ithaca + vedle ni vytvori
# license.ithaca (plaintext JSON s identitou vlastnika). Klic se odvodi z
# master secretu ($(SECRET_FILE)) + license. Detaily: docs/bank-format-packed.md.
#   make new-license SRC=<dynamicka-banka> DST=<cilovy-adresar>
#     -> interaktivni dotaz na udaje vlastnika (email/jmeno/transakce)
#   make new-license SRC=... DST=... LICENSE_JSON=<json>
#     -> neinteraktivni (udaje z JSON souboru)
.PHONY: new-license
new-license: ## licencovana (sifrovana) banka + license.ithaca [SRC=<dyn-banka> DST=<cil>]
	@if [ -z "$(SRC)" ] || [ -z "$(DST)" ]; then \
	    printf "usage: make new-license SRC=<dynamicka-banka> DST=<cilovy-adresar> [LICENSE_JSON=<json>]\n"; \
	    exit 1; \
	fi
	@python3 tools/gen-bank-secret.py $(SECRET_FILE)
	@python3 tools/bake_soundbank.py \
	    --source-soundbank-dir "$(SRC)" \
	    --destination-soundbank-dir "$(DST)" \
	    --secret-file "$(SECRET_FILE)" \
	    $(if $(LICENSE_JSON),--license-json "$(LICENSE_JSON)",--license) \
	    --verify
