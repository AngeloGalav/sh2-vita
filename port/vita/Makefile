# The PS Vita build: the same game sources as the 3DS build (../source), compiled against the
# compatibility layer in compat/ (the citro3d / libctru calls the game makes, on vitaGL and the
# Sce* libraries). Run through build.sh, which supplies VitaSDK in Docker.

ifeq ($(strip $(VITASDK)),)
$(error "VITASDK is not set: run port/vita/build.sh")
endif

TARGET    := sh2vita
TITLE     := Silent Hill 2
TITLE_ID  := SHIL00002
VERSION   := 00.30

PREFIX    := arm-vita-eabi
CC        := $(PREFIX)-gcc
BUILD     := build

GAME_SRC  := $(wildcard ../source/*.c) $(wildcard ../source/game/view/*.c)
COMPAT_SRC:= $(wildcard compat/*.c)
# The vertex programs are GL translations in compat/c3d_shaders.c; each *_shbin.h only names
# its program, so the game's DVLB_ParseFile(world_shbin, ...) picks the translation.
PICA      := $(notdir $(basename $(basename $(wildcard ../source/*.v.pica))))
GEN       := $(patsubst %,$(BUILD)/gen/%_shbin.h,$(PICA))

OBJS      := $(patsubst ../source/%.c,$(BUILD)/game/%.o,$(GAME_SRC)) \
             $(patsubst compat/%.c,$(BUILD)/compat/%.o,$(COMPAT_SRC))

# compat/include comes first so <3ds.h>, <citro3d.h> and <tex3ds.h> resolve to the layer
CFLAGS    := -O2 -g -Wall -Wno-unused-function -ffast-math -fno-strict-aliasing \
             -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb \
             -ffunction-sections -fdata-sections \
             -D__vita__ -DSH2_VITA \
             -Icompat/include -Ideps/vitagl/include -I../source -I$(BUILD)/gen -MMD -MP

LIBS      := -lvitaGL -lvitashark -lSceShaccCgExt -lmathneon -ltaihen_stub \
             -lSceShaccCg_stub -lSceKernelDmacMgr_stub -lSceGxm_stub -lSceDisplay_stub \
             -lSceCtrl_stub -lSceTouch_stub -lSceAudio_stub -lSceAppMgr_stub -lSceAppUtil_stub \
             -lSceCommonDialog_stub -lSceSysmodule_stub -lScePower_stub -lSceIme_stub \
             -lSceRtc_stub -lSceLibKernel_stub -lSceKernelThreadMgr_stub -lstdc++ -lpthread -lm -lc

# the game's sdmc: paths go through compat/ctru.c's vita_path
LDFLAGS   := -Ldeps/vitagl -Wl,-q -Wl,--gc-sections \
             -Wl,--wrap=main,--wrap=fopen,--wrap=opendir,--wrap=stat,--wrap=mkdir,--wrap=remove,--wrap=rename

all: $(TARGET).vpk

$(TARGET).vpk: eboot.bin $(BUILD)/param.sfo sce_sys/icon0.png
	vita-pack-vpk -s $(BUILD)/param.sfo -b eboot.bin \
	  -a sce_sys/icon0.png=sce_sys/icon0.png \
	  -a sce_sys/livearea/contents/bg.png=sce_sys/livearea/contents/bg.png \
	  -a sce_sys/livearea/contents/startup.png=sce_sys/livearea/contents/startup.png \
	  -a sce_sys/livearea/contents/template.xml=sce_sys/livearea/contents/template.xml $@

eboot.bin: $(TARGET).velf
	vita-make-fself -c $< $@

$(TARGET).velf: $(TARGET).elf
	vita-elf-create $< $@

$(TARGET).elf: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD)/param.sfo:
	@mkdir -p $(BUILD)
	vita-mksfoex -s TITLE_ID=$(TITLE_ID) -s APP_VER=$(VERSION) -d ATTRIBUTE2=12 "$(TITLE)" $@

$(BUILD)/gen/%_shbin.h:
	@mkdir -p $(dir $@)
	printf 'static const unsigned char %s_shbin[] = "%s";\nstatic const unsigned int %s_shbin_size = sizeof %s_shbin;\n' $* $* $* $* > $@

$(BUILD)/game/%.o: ../source/%.c | $(GEN)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/compat/%.o: compat/%.c | $(GEN)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

check: $(GEN)
	@for f in $(GAME_SRC); do $(CC) $(CFLAGS) -fsyntax-only $$f 2>&1; done | grep -E 'error|warning: implicit' | sort | uniq -c | sort -rn

clean:
	rm -rf $(BUILD) $(TARGET).elf $(TARGET).velf eboot.bin $(TARGET).vpk

.PHONY: all clean check
-include $(OBJS:.o=.d)
