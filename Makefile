#---------------------------------------------------------------------------------
# WiiFin U: WiiFin for the Wii U (devkitPPC, wut), an Aroma app.
#
#   make                             -> WiiFinU.wuhb (and WiiFinU.rpx)
#
# WiiFin's shared sources (from upstream, fabienmillet/WiiFin) are built as
# they are, against source/platform/wiiu: compat/ gives them libogc's and
# GRRLIB's headers, the other files there implement them on the Wii U (see
# source/platform/wiiu/README.md).
# Requirements: tools/wiiu/setup.sh (FFmpeg into libs/ffmpeg-wiiu, mbedTLS
# into libs/mbedtls/libwiiu).
#---------------------------------------------------------------------------------
.SUFFIXES:

DEVKITPRO   ?= /opt/devkitpro
DEVKITPPC   ?= $(DEVKITPRO)/devkitPPC
WUT_ROOT    := $(DEVKITPRO)/wut
PORTLIBS    := $(DEVKITPRO)/portlibs
PREFIX      := $(DEVKITPPC)/bin/powerpc-eabi-
CC          := $(PREFIX)gcc
CXX         := $(PREFIX)g++
TOOLS       := $(DEVKITPRO)/tools/bin

TARGET      := WiiFinU
BUILD       := build
APP_NAME    ?= WiiFin U
APP_SHORT   ?= WiiFin U
APP_AUTHOR  := Fabien MILLET

PWU         := source/platform/wiiu
SOURCES     := source source/core source/input source/ui source/jellyfin source/player $(PWU) $(PWU)/ui

CPPFILES    := $(foreach dir,$(SOURCES),$(wildcard $(dir)/*.cpp))
CFILES      := $(foreach dir,$(SOURCES),$(wildcard $(dir)/*.c))

SRC_OFILES  := $(addprefix $(BUILD)/,$(notdir $(CPPFILES:.cpp=.o)) $(notdir $(CFILES:.c=.o)))
DEPFILES    := $(SRC_OFILES:.o=.d)

ASSET_OFILES := \
	$(BUILD)/wii_font_ttf.o \
	$(BUILD)/jp_font_ttf.o \
	$(BUILD)/button_start_png.o \
	$(BUILD)/logo_wiifin_png.o \
	$(BUILD)/cursor_pointer_png.o \
	$(BUILD)/cursor_hand_open_png.o \
	$(BUILD)/cursor_hand_closed_png.o \
	$(BUILD)/ring_png.o \
	$(BUILD)/cacert_pem.o \
	$(BUILD)/music_mp3.o \
	$(BUILD)/fx_start_mp3.o \
	$(BUILD)/fx_select_mp3.o \
	$(BUILD)/fx_press_key_mp3.o \
	$(BUILD)/fx_menu_exit_mp3.o \
	$(BUILD)/fx_menu_enter_mp3.o \
	$(BUILD)/fx_loading_mp3.o \
	$(BUILD)/fx_backspace_mp3.o \
	$(BUILD)/fx_back_mp3.o \
	$(BUILD)/lang_mo.o

OFILES      := $(ASSET_OFILES) $(SRC_OFILES)

MACHDEP     := -DESPRESSO -mcpu=750 -meabi -mhard-float
INCLUDE     := -I$(CURDIR)/$(PWU)/compat \
               $(foreach dir,$(SOURCES),-I$(CURDIR)/$(dir)) -I$(CURDIR)/$(BUILD) \
               -I$(CURDIR)/libs/mbedtls/include -I$(CURDIR)/libs/ffmpeg-wiiu/include \
               -I$(PORTLIBS)/wiiu/include -I$(PORTLIBS)/ppc/include -I$(PORTLIBS)/ppc/include/freetype2 \
               -I$(PORTLIBS)/ppc/include/libpng16 -I$(WUT_ROOT)/include

WERROR      ?=
# -fno-if-conversion: no conditional traps (tweq, from libstdc++'s checks
# such as shared_ptr's count overflow): Cemu's recompiler loops forever on
# them (WiiFin froze on sign-in there); the console runs either
CFLAGS      := -g -O2 -fno-if-conversion -fno-if-conversion2 -Wall $(WERROR) -MMD -MP -ffunction-sections -fdata-sections \
               $(MACHDEP) -D__WIIU__ -D__WUT__ -DAPP_NAME='"$(APP_NAME)"' $(INCLUDE)
CXXFLAGS    := $(CFLAGS) -fno-rtti -std=gnu++17
LDFLAGS     := -specs=$(WUT_ROOT)/share/wut.specs -g $(MACHDEP) -Wl,-Map,$(TARGET).map -Wl,--gc-sections \
               -Wl,--wrap=SYS_Report -Wl,--wrap=GRRLIB_Render
LIBS        := -L$(CURDIR)/libs/ffmpeg-wiiu/lib -lavformat -lavcodec -lswscale -lswresample -lavutil \
               -L$(CURDIR)/libs/mbedtls/libwiiu -lmbedtls -lmbedx509 -lmbedcrypto \
               -L$(PORTLIBS)/wiiu/lib -lSDL2 \
               -L$(PORTLIBS)/ppc/lib -lfreetype -lpng16 -lbrotlidec -lbrotlicommon -lbz2 -ljpeg -lmad -lz \
               -L$(WUT_ROOT)/lib -lwut -lm

.PHONY: all clean

all: $(BUILD) $(TARGET).wuhb

$(BUILD):
	mkdir -p $@

$(TARGET).wuhb: $(TARGET).rpx $(PWU)/meta/icon.png $(PWU)/meta/splash_tv.png $(PWU)/meta/splash_drc.png
	$(TOOLS)/wuhbtool $< $@ --name "$(APP_NAME)" --short-name "$(APP_SHORT)" --author "$(APP_AUTHOR)" \
		--icon=$(PWU)/meta/icon.png --tv-image=$(PWU)/meta/splash_tv.png --drc-image=$(PWU)/meta/splash_drc.png
	@echo built ... $@

# no conditional trap may be left (see CFLAGS): `trap` and `twu` (always
# taken: a bug, abort) are the only ones allowed
$(TARGET).rpx: $(TARGET).elf
	@T=$$($(PREFIX)objdump -d --no-show-raw-insn $< | grep -P '\ttw(?!u )[a-z]*i? ' | head -5); \
	if [ -n "$$T" ]; then echo "conditional traps (Cemu loops on them):"; echo "$$T"; exit 1; fi
	$(TOOLS)/elf2rpl $< $@

$(TARGET).elf: $(OFILES)
	$(CXX) $^ -o $@ $(LDFLAGS) $(LIBS)

clean:
	rm -rf $(BUILD) $(TARGET).wuhb $(TARGET).rpx $(TARGET).elf $(TARGET).map

# Sources
vpath %.cpp $(SOURCES)
vpath %.c $(SOURCES)
$(BUILD)/%.o: %.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@
$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# Embedded files (WiiFin's fonts, pictures, sounds)
define EMBED
$(BUILD)/$(1).o: $(2) | $(BUILD)
	xxd -i -n $(3) $$< > $(BUILD)/$(1).c
	$(CC) $(MACHDEP) -c -o $$@ $(BUILD)/$(1).c
endef
$(eval $(call EMBED,cursor_pointer_png,data/cursors/PointerP1-64.png,data_cursors_PointerP1_64_png))
$(eval $(call EMBED,cursor_hand_open_png,data/cursors/HandOpenP1-64.png,data_cursors_HandOpenP1_64_png))
$(eval $(call EMBED,cursor_hand_closed_png,data/cursors/HandClosedP1-64.png,data_cursors_HandClosedP1_64_png))
$(eval $(call EMBED,button_start_png,data/images/button_start.png,data_button_start_png))
# WiiFin U's logo (WiiFin's, with its U) under WiiFin's name: the screens draw it as they are
$(eval $(call EMBED,logo_wiifin_png,data/images/logo_wiifinu.png,data_logo_wiifin_png))
$(eval $(call EMBED,wii_font_ttf,data/fonts/wii_font.ttf,data_wii_font_ttf))
$(eval $(call EMBED,jp_font_ttf,data/fonts/jp_font.ttf,data_jp_font_ttf))
$(eval $(call EMBED,ring_png,data/images/ring.png,data_ring_png))
$(eval $(call EMBED,music_mp3,data/sounds/bgm.mp3,data_music_mp3))
$(eval $(call EMBED,fx_start_mp3,data/sounds/start.mp3,data_sounds_start_mp3))
$(eval $(call EMBED,fx_select_mp3,data/sounds/select.mp3,data_sounds_select_mp3))
$(eval $(call EMBED,fx_back_mp3,data/sounds/back.mp3,data_sounds_back_mp3))
$(eval $(call EMBED,fx_backspace_mp3,data/sounds/backspace.mp3,data_sounds_backspace_mp3))
$(eval $(call EMBED,fx_loading_mp3,data/sounds/loading.mp3,data_sounds_loading_mp3))
$(eval $(call EMBED,fx_menu_enter_mp3,data/sounds/menu_enter.mp3,data_sounds_menu_enter_mp3))
$(eval $(call EMBED,fx_menu_exit_mp3,data/sounds/menu_exit.mp3,data_sounds_menu_exit_mp3))
$(eval $(call EMBED,fx_press_key_mp3,data/sounds/press_key.mp3,data_sounds_press_key_mp3))

$(BUILD)/cacert_pem.o: data/certs/cacert.pem | $(BUILD)
	(cat $<; printf '\0') > $(BUILD)/cacert_pem_null.pem
	xxd -i -n data_cacert_pem $(BUILD)/cacert_pem_null.pem > $(BUILD)/cacert_pem.c
	$(CC) $(MACHDEP) -c -o $@ $(BUILD)/cacert_pem.c

# the translations (lang/*.po)
LANG_PO := $(sort $(wildcard lang/*.po))
$(BUILD)/lang_mo.o: $(LANG_PO) tools/lang_table.sh | $(BUILD)
	bash tools/lang_table.sh $(BUILD) $(LANG_PO) > $(BUILD)/lang_mo.c
	$(CC) $(MACHDEP) -c -o $@ $(BUILD)/lang_mo.c

-include $(DEPFILES)
