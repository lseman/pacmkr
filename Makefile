BUILD_DIR ?= build
BUILD_TYPE ?= Release
PREFIX ?= /usr
GUI ?= ON
JOBS ?= $(shell getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)

.PHONY: all configure build test install install-user clean help

all: build

configure:
	cmake -S . -B "$(BUILD_DIR)" \
		-DCMAKE_BUILD_TYPE="$(BUILD_TYPE)" \
		-DCMAKE_INSTALL_PREFIX="$(PREFIX)" \
		-DPACMKR_BUILD_GTK_GUI="$(GUI)" \
		-DPACMKR_BUILD_TESTS=ON

build: configure
	cmake --build "$(BUILD_DIR)" --parallel "$(JOBS)"

test: build
	ctest --test-dir "$(BUILD_DIR)" --output-on-failure

install: build
	cmake --install "$(BUILD_DIR)"

install-user:
	$(MAKE) install PREFIX="$(HOME)/.local"

clean:
	cmake -E remove_directory "$(BUILD_DIR)"

help:
	@echo "pacmkr build targets"
	@echo "  make              Build CLI and GTK GUI"
	@echo "  make test         Build and run tests"
	@echo "  make install      Install under PREFIX (default: /usr/local)"
	@echo "  make install-user Install under ~/.local without root"
	@echo "  make clean        Remove BUILD_DIR"
	@echo ""
	@echo "Options: GUI=OFF BUILD_TYPE=Debug PREFIX=/usr BUILD_DIR=build"
