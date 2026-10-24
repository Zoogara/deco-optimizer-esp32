# Host-side renderer: builds the real firmware UI source (main/ui.c) and LVGL 8.4 for this computer and renders
# screens with invented demo data to PNG files. Not part of the firmware build.
#   make -C tools/screenshots shots     # build ./render, write PNGs to ./out
# Needs: a C compiler, Python 3 with Pillow, and a normal `idf.py build` first (it fetches LVGL into
# managed_components/ and writes build/config/sdkconfig.h, which supplies the LVGL settings).

ROOT    := $(abspath ../..)
LVGL    ?= $(ROOT)/managed_components/lvgl__lvgl
BUILD   := build
CC      ?= cc
CFLAGS  := -O1 -g -w -std=gnu11 -MMD -MP -DLV_CONF_KCONFIG_EXTERNAL_INCLUDE=\"sdkconfig.h\" -DHOST_RENDER \
           -Ishim -I$(LVGL) -I$(LVGL)/src -I$(ROOT)/main

LVGL_DIRS := core draw draw/sw hal misc widgets font extra extra/themes extra/themes/default \
             extra/layouts extra/layouts/flex extra/layouts/grid extra/others extra/libs/qrcode
LVGL_SRC  := $(foreach d,$(LVGL_DIRS),$(wildcard $(LVGL)/src/$(d)/*.c)) $(wildcard $(LVGL)/src/extra/widgets/*/*.c)
APP_SRC   := render.c demo_data.c host_ui.c

OBJS := $(patsubst %.c,$(BUILD)/%.o,$(subst $(LVGL)/,lvgl/,$(LVGL_SRC)) $(APP_SRC))

all: render

# LVGL gets the same settings as the device (the CONFIG_LV_* part of the firmware's sdkconfig.h), except the
# colour depth: the device uses 16-bit swapped RGB565, the host renders 32-bit, which looks identical.
shim/sdkconfig.h: $(ROOT)/build/config/sdkconfig.h
	grep '^#define CONFIG_LV_' $< | grep -v 'COLOR_DEPTH\|COLOR_16_SWAP' > $@
	printf '#define CONFIG_LV_COLOR_DEPTH_32 1\n#define CONFIG_LV_COLOR_DEPTH 32\n' >> $@

$(BUILD)/%.o: %.c shim/sdkconfig.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/lvgl/%.o: $(LVGL)/%.c shim/sdkconfig.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

-include $(OBJS:.o=.d)

render: $(OBJS)
	$(CC) $(OBJS) -lm -o $@

shots: render
	@mkdir -p out
	./render out
	python3 topng.py out

web:
	python3 web_shots.py out

clean:
	rm -rf $(BUILD) render out shim/sdkconfig.h
