PIO ?= $(shell if [ -x ../yak-cyd/.venv/bin/pio ]; then echo ../yak-cyd/.venv/bin/pio; else echo pio; fi)
PYTHON ?= python3
CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -g -Wall -Wextra -Wno-unused-parameter -Isrc -DTT_HOST_BUILD=1

VM_SRCS = \
	src/vm/tt_memory.cpp \
	src/vm/tt_compiler.cpp \
	src/vm/tt_vm.cpp \
	src/vm/tt_prims.cpp \
	src/vm/tt_world.cpp

UI_SRCS = \
	src/hal/hal_mock.cpp \
	src/ui/tt_pen.cpp \
	src/ui/tt_graffiti.cpp \
	src/ui/tt_keyboard.cpp \
	src/ui/tt_wm.cpp \
	src/ui/tt_ide.cpp \
	src/ui/tt_serial.cpp

PRELUDE_HDR = src/vm/prelude_rodata.h

.PHONY: all embed test test-vm test-ui build fs upload uploadfs monitor clean

all: test build fs

$(PRELUDE_HDR): data/prelude.tt tools/embed_prelude.py
	$(PYTHON) tools/embed_prelude.py data/prelude.tt $(PRELUDE_HDR)

embed: $(PRELUDE_HDR)

test-vm: $(PRELUDE_HDR)
	mkdir -p build
	$(CXX) $(CXXFLAGS) $(VM_SRCS) test/test_vm.cpp -o build/test_vm -lm
	./build/test_vm

test-ui: $(PRELUDE_HDR)
	mkdir -p build
	$(CXX) $(CXXFLAGS) $(VM_SRCS) $(UI_SRCS) test/test_ui.cpp -o build/test_ui -lm
	./build/test_ui

test: test-vm test-ui

build: $(PRELUDE_HDR)
	$(PIO) run -e cyd

fs: $(PRELUDE_HDR)
	$(PIO) run -e cyd -t buildfs

upload: $(PRELUDE_HDR)
	$(PIO) run -e cyd -t upload

uploadfs: $(PRELUDE_HDR)
	$(PIO) run -e cyd -t uploadfs

monitor:
	$(PIO) device monitor -b 115200

clean:
	rm -rf build .pio
