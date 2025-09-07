# =============================================================================
# Spacedrop (C edition) — Makefile
#
# This Makefile is a thin wrapper to:
#   1) Ensure CivetWeb submodule is present at C_Release/third_party/civetweb
#   2) Pin it to a known-good version tag (CIVETWEB_REF)
#   3) Run CMake to build the demo server ("hello")
#
# Usage:
#   make submodule        # fetch/initialize civetweb at the pinned tag
#   make build            # configure + compile (TLS/ZLIB default OFF)
#   make run              # run ./build/hello
#   make clean            # remove cmake’s build artifacts
#   make distclean        # also wipe the vendored civetweb (careful)
#
# Feature toggles (pass as env or make VAR=...):
#   TLS=ON|OFF   (default OFF)
#   ZLIB=ON|OFF  (default OFF)
#
# Examples:
#   make submodule
#   make build TLS=ON ZLIB=ON
#   make run
# =============================================================================

# Path to this module’s root (the C_Release folder)
ROOT := $(abspath .)

# Where the CivetWeb submodule lives (inside this folder, per your request)
CVW_DIR := $(ROOT)/third_party/civetweb
CVW_URL := https://github.com/civetweb/civetweb.git

# Version lock: as of 2025-09-07, the latest official release tag is v1.16
# If a newer tag appears, update this one line.
CIVETWEB_REF := v1.16

# Build directory (out-of-source)
BUILD_DIR := $(ROOT)/build

# Feature flags (default OFF; can be overridden: make build TLS=ON ZLIB=ON)
TLS  ?= OFF
ZLIB ?= OFF

.PHONY: all help submodule update build run clean distclean _cmake_config _cmake_build

all: clean submodule build run

help:
	@echo "Usage: make [target] [VAR=VALUE]"
	@echo ""
	@echo "Targets:"
	@echo "  all (default)  - clean, submodule, build, run"
	@echo "  submodule      - ensure CivetWeb submodule is present and at the pinned tag"
	@echo "  update         - update CivetWeb submodule to the pinned tag"
	@echo "  build          - configure + compile the demo server (depends on submodule)"
	@echo "  run            - run the demo server (depends on build)"
	@echo "  clean          - remove cmake’s build artifacts"
	@echo "  distclean      - also wipe the vendored civetweb (careful)"
	@echo ""
	@echo "Feature flags (can be passed as VAR=VALUE):"
	@echo "  TLS=ON|OFF    (default OFF)"
	@echo "  ZLIB=ON|OFF   (default OFF)"
	@echo ""
	@echo "Examples:"
	@echo "  make submodule"
	@echo "  make build TLS=ON ZLIB=ON"
	@echo "  make run"
	@echo ""

# --- 1) Submodule bootstrap ---------------------------------------------------
submodule:
	@echo "==> Ensuring CivetWeb submodule at $(CVW_DIR)"
	@if [ ! -d "$(CVW_DIR)/.git" ]; then \
	  echo "   -> Adding submodule"; \
	  git submodule add "$(CVW_URL)" "third_party/civetweb" || true; \
	fi
	@git submodule update --init --recursive "third_party/civetweb"
	@echo "   -> Checking out $(CIVETWEB_REF)"
	@cd "$(CVW_DIR)" && git fetch --tags && git checkout --detach "$(CIVETWEB_REF)"
	@HASH=$$(cd "$(CVW_DIR)" && git rev-parse --short HEAD); echo "   -> CivetWeb pinned at $$HASH"

# (Optional) Bring submodule to the pinned tag again
update:
	@echo "==> Updating CivetWeb to $(CIVETWEB_REF)"
	@cd "$(CVW_DIR)" && git fetch --tags && git checkout --detach "$(CIVETWEB_REF)"
	@echo "   -> CivetWeb now at $$((cd "$(CVW_DIR)" && git rev-parse --short HEAD))"

# --- 2) Configure + build via CMake ------------------------------------------
build: submodule _cmake_config _cmake_build
	@echo "==> Build complete. Binary: $(BUILD_DIR)/hello"

_cmake_config:
	@mkdir -p "$(BUILD_DIR)"
	@cmake -S "$(ROOT)" -B "$(BUILD_DIR)" \
	  -DSPACEDROP_ENABLE_TLS=$(TLS) \
	  -DSPACEDROP_ENABLE_ZLIB=$(ZLIB) \
	  -DCIVETWEB_DIR="$(CVW_DIR)"

_cmake_build:
	@cmake --build "$(BUILD_DIR)" --config Release -j

# --- 3) Run the demo server ---------------------------------------------------
run:
	@echo "==> Running hello server (Ctrl+C to stop)"
	@"$(BUILD_DIR)/hello"

# --- Cleanup -----------------------------------------------------------------
clean:
	@echo "==> Cleaning build artifacts"
	@rm -rf "$(BUILD_DIR)"

distclean: clean
	@echo "==> Removing vendored CivetWeb (submodule dir only; does not change .gitmodules)"
	@rm -rf "$(CVW_DIR)"