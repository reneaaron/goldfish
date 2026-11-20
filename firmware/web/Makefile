# Browser simulator: src/app + src/web compiled to WebAssembly with Emscripten,
# with the page around it (try/).
#
#   pio pkg install -e sim          # once: fetches LVGL and libwally into .pio/libdeps/sim
#   make -C web -j                  # -> .pio/build/web/{index.html,signer.js,signer.wasm,samples/}
#   python3 -m http.server -d .pio/build/web
#
# Needs emcc on PATH (emsdk). Same sources and libraries as the sim env in
# platformio.ini, with src/web in place of src/sim.

FW      := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
DEPS    := $(FW)/.pio/libdeps/sim
LVGL    := $(DEPS)/lvgl
WALLY   := $(DEPS)/libwally-core
OUT     := $(FW)/.pio/build/web
OBJ     := $(OUT)/obj

CC      := emcc
CXX     := em++
OPT     := -O2
DEFS    := -DLV_CONF_INCLUDE_SIMPLE -DWALLY_ABI_NO_ELEMENTS
INCS    := -I$(FW)/include -I$(LVGL) -I$(WALLY)/include -I$(FW)/lib/quirc/src
CFLAGS  := $(OPT) $(DEFS) $(INCS)

APP_SRC   := $(wildcard $(FW)/src/app/*.cpp $(FW)/src/app/fonts/*.c $(FW)/src/app/images/*.c) $(FW)/src/web/main.cpp
LVGL_SRC  := $(shell find $(LVGL)/src -name '*.c')
QUIRC_SRC := $(wildcard $(FW)/lib/quirc/src/*.c)
WALLY_SRC := $(FW)/lib/wally/wally_amalgamation.c

OBJS := $(patsubst $(FW)/%,$(OBJ)/%.o,$(APP_SRC) $(QUIRC_SRC) $(WALLY_SRC)) \
        $(patsubst $(DEPS)/%,$(OBJ)/deps/%.o,$(LVGL_SRC))

LDFLAGS := $(OPT) -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=16MB -sSTACK_SIZE=1MB \
           -sMODULARIZE=1 -sEXPORT_NAME=Signer -sENVIRONMENT=web \
           -sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAPU16 -sFILESYSTEM=0

.PHONY: all clean
all: $(OUT)/signer.js $(OUT)/index.html $(OUT)/img/handwritten_page_thumb.jpg samples

$(OUT)/signer.js: $(OBJS)
	$(CXX) $(LDFLAGS) $^ -o $@

$(OUT)/index.html: $(FW)/../try/index.html
	@mkdir -p $(@D)
	cp $< $@

$(OUT)/img/%: $(FW)/../try/img/%
	@mkdir -p $(@D)
	cp $< $@

SAMPLES := testseed_height_demise_standard_12word.png testpsbt_height_demise_base64.png \
           mainpsbt_height_demise_base64.png handwritten_page.png
samples: $(addprefix $(OUT)/samples/,$(SAMPLES))
$(OUT)/samples/%: $(FW)/sim/samples/%
	@mkdir -p $(@D)
	cp $< $@

# The version the home screen shows, as version.py writes it for the pio builds.
$(FW)/include/version.h: FORCE
	@v=$$(git -C $(FW) describe --tags --exact-match 2>/dev/null || git -C $(FW) rev-parse --short=7 HEAD); \
	 if [ -n "$$(git -C $(FW) status --porcelain --untracked-files=no)" ]; then v="$$v+"; fi; \
	 c="#pragma once\n#define FW_VERSION \"$$v\""; \
	 if [ "$$(printf "$$c\n")" != "$$(cat $@ 2>/dev/null)" ]; then printf "$$c\n" > $@; fi
FORCE:

$(OBJ)/src/app/ui.cpp.o: $(FW)/include/version.h

$(OBJ)/%.cpp.o: $(FW)/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(CFLAGS) -std=gnu++17 -c $< -o $@
$(OBJ)/%.c.o: $(FW)/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@
$(OBJ)/lib/quirc/%.c.o: $(FW)/lib/quirc/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -DQUIRC_MAX_REGIONS=254 -c $< -o $@
$(OBJ)/lib/wally/%.c.o: $(FW)/lib/wally/%.c
	@mkdir -p $(@D)
	$(CC) $(OPT) -DBUILD_MINIMAL -DWALLY_ABI_NO_ELEMENTS -I$(FW)/lib/wally/config -I$(WALLY) -I$(WALLY)/src \
	      -I$(WALLY)/src/ccan -I$(WALLY)/src/secp256k1/include -I$(WALLY)/include -c $< -o $@
$(OBJ)/deps/%.c.o: $(DEPS)/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(OUT)
