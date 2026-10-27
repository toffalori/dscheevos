# DScheevos launcher (BlocksDS). Build:
#   docker run --rm -v $PWD:/repo -w /repo/dscheevos/launcher skylyrac/blocksds:slim-latest make
NAME          := dscheevos-launcher
# Two banner lines: BlocksDS joins TITLE;AUTHOR when the subtitle is empty, so
# the tagline goes in GAME_AUTHOR (a third "Cheevotron" line looked doubled).
GAME_TITLE    := Cheevotron
GAME_SUBTITLE :=
GAME_AUTHOR   := Achievements on real hardware
GAME_ICON     := icon.bmp

SOURCEDIRS    := source rcheevos tls
INCLUDEDIRS   := ../module/include ../../external/rcheevos/include ../../external/rcheevos/src \
                 tls ../../external/mbedtls/include
DEFINES       := -DRC_NO_THREADS -DRC_CLIENT_SUPPORTS_HASH -DRC_HASH_NO_DISC -DRC_HASH_NO_ZIP \
                 -DRC_HASH_NO_ENCRYPTED -DNDEBUG '-DMBEDTLS_CONFIG_FILE="dsc_mbedtls_config.h"' \
                 '-DDSC_VERSION="$(shell cat ../../VERSION)"'
ARM7ELF       := arm7/build/arm7.elf
LIBS          := -ldswifi9 -lnds9
LIBDIRS       := $(BLOCKSDS)/libs/dswifi $(BLOCKSDS)/libs/libnds

include $(BLOCKSDS)/sys/default_makefiles/rom_arm9/Makefile

# our ARM7 core: DSWiFi plus the handover freeze
$(ROM): $(ARM7ELF)
$(ARM7ELF): $(wildcard arm7/source/*.c)
	@+$(MAKE) -C arm7 --no-print-directory
clean: clean-arm7
.PHONY: clean-arm7
clean-arm7:
	@$(MAKE) -C arm7 clean --no-print-directory
