# Browser simulator: src/app + src/web compiled to WebAssembly with Emscripten.
# The landing page's hero runs it ("Plug it in") from the site's /try/, where this
# build goes, with try/sim.js; try/index.html only sends old links to the hero.
#
#   pio pkg install -e sim          # once: fetches LVGL and libwally into .pio/libdeps/sim
#   make -C web -j                  # -> .pio/build/web/{signer.js,signer.wasm,sim.js,img/,samples/}
#   mkdir -p /tmp/site && cp -rL ../landing/. /tmp/site && cp -r .pio/build/web /tmp/site/try
#   python3 -m http.server -d /tmp/site
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
CFLAGS  := $(OPT) $(DEFS) $(INCS) -MMD -MP   # -MMD: a header change rebuilds what includes it

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
all: $(OUT)/signer.js $(OUT)/index.html $(OUT)/sim.js $(OUT)/img/camera_idle.jpg samples

$(OUT)/signer.js: $(OBJS)
	$(CXX) $(LDFLAGS) $^ -o $@

$(OUT)/index.html: $(FW)/../try/index.html
	@mkdir -p $(@D)
	cp $< $@

$(OUT)/sim.js: $(FW)/../try/sim.js
	@mkdir -p $(@D)
	cp $< $@

# what the camera sees with no picture picked (sim.js shows it)
$(OUT)/img/%: $(FW)/../try/img/%
	@mkdir -p $(@D)
	cp $< $@

SAMPLES := testseed_height_demise_standard_12word.png mainpsbt_height_demise_base64.png \
           handwritten_page_demo.png
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

-include $(OBJS:.o=.d)

clean:
	rm -rf $(OUT)
