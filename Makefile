#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

#---------------------------------------------------------------------------------
# TARGET is the name of the output
# BUILD is the directory where object files & dependancy files will be placed
# SOURCES is a list of directories containing source code
# DATA is a list of directories containing data files
# INCLUDES is a list of directories containing header files
#
# NO_SMDH: if set to anything, no SMDH file is generated.
# ROMFS is the directory which contains the RomFS, relative to the Makefile (Optional)
# APP_TITLE is the name of the app stored in the SMDH file (Optional)
# APP_AUTHOR is the author of the app stored in the SMDH file (Optional)
# APP_VERSION is the version of the app stored in the SMDH file (Optional)
# APP_TITLEID is the titleID of the app stored in the CIA
# APP_ICON is the filename of the icon (.png), relative to the project folder.
#---------------------------------------------------------------------------------
TARGET		:=	gen1recomp3ds
BUILD		:=	build
SOURCES		:=	source
DATA		:=	data
INCLUDES	:=	include
ROMFS		:=	romfs

APP_TITLE	:=	gen1recomp
APP_AUTHOR	:=	marvynmesquita
APP_VERSION	:=	1.0

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
ARCH	:=	-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS	:=	-g -Wall -O2 -mword-relocations \
			-fomit-frame-pointer -ffast-math \
			$(ARCH)

CFLAGS	+=	$(INCLUDE) -D__3DS__

CXXFLAGS	:= $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++11

ASFLAGS	:=	-g $(ARCH)
LDFLAGS	=	-specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

#---------------------------------------------------------------------------------
# any extra libraries we wish to link with the project
#---------------------------------------------------------------------------------
LIBS	:= -lcitro2d -llua5.1 -lcitro3d -lctru -lm

#---------------------------------------------------------------------------------
# list of directories containing libraries, this must be the top level containing
# include and lib
#---------------------------------------------------------------------------------
LIBDIRS	:= $(PORTLIBS) $(CTRULIB)


#---------------------------------------------------------------------------------
# no real need to edit anything past this point unless you need to add additional
# rules for different file extensions
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
			$(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
PICAFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.v.pica)))
SHLISTFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.shlist)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

#---------------------------------------------------------------------------------
# use CXX for linking C++ projects, CC for standard C
#---------------------------------------------------------------------------------
ifeq ($(strip $(CPPFILES)),)
#---------------------------------------------------------------------------------
	export LD	:=	$(CC)
#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------
	export LD	:=	$(CXX)
#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------

export OFILES_BIN	:=	$(addsuffix .o,$(BINFILES))
export OFILES_SRC	:=	$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES_PICA	:=	$(PICAFILES:.v.pica=.shbin.o) $(SHLISTFILES:.shlist=.shbin.o)
export OFILES 	:=	$(OFILES_BIN) $(OFILES_SRC) $(OFILES_PICA)

export HFILES	:=	$(PICAFILES:.v.pica=.shbin.h) $(SHLISTFILES:.shlist=.shbin.h)

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include -I$(dir)/include/lua5.1) \
			-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

ifeq ($(strip $(ICON)),)
	icons := $(wildcard *.png)
	ifneq (,$(findstring $(TARGET).png,$(icons)))
		export APP_ICON := $(TOPDIR)/$(TARGET).png
	else
		ifneq (,$(findstring icon.png,$(icons)))
			export APP_ICON := $(TOPDIR)/icon.png
		endif
	endif
else
	export APP_ICON := $(TOPDIR)/$(ICON)
endif

ifeq ($(strip $(NO_SMDH)),)
	export _3DSXFLAGS += --smdh=$(CURDIR)/$(TARGET).smdh
endif

ifneq ($(ROMFS),)
	export _3DSXFLAGS += --romfs=$(CURDIR)/$(ROMFS)
endif

.PHONY: $(BUILD) clean all cia

#---------------------------------------------------------------------------------
all: $(BUILD)

#-------------------------------------------------------------------------------
# cia: builds the CIA with a valid banner (CBMD) and logo (darc/LZ11) in the
# ExeFS.  Both files must be present with the correct binary format or the
# Home Menu / NS will kill the title at launch (ErrDisp "The SD Card was
# removed").  The logo is the standard Nintendo boot logo (LZ11-compressed
# darc); the banner is generated with tools/bannertool (CBMD 256x128 + CWAV).
#-------------------------------------------------------------------------------
cia:
	@$(MAKE) all
	@$(MAKE) build-logo build-banner
	@echo "Building CIA..."
	@makerom -f cia -o $(CURDIR)/$(TARGET).cia -elf $(CURDIR)/$(TARGET).elf -icon $(CURDIR)/$(TARGET).smdh -banner $(BUILD)/banner.bin -logo $(BUILD)/logo.bin -rsf $(CURDIR)/$(TARGET).rsf -target t -exefslogo -v 2>&1
	@echo "CIA built: $(TARGET).cia"

build-logo:
	@mkdir -p $(BUILD)
	@if [ -f $(CURDIR)/assets/logo.bin ]; then cp $(CURDIR)/assets/logo.bin $(BUILD)/logo.bin; else cp /tmp/uu_exefs/logo.bin $(BUILD)/logo.bin; fi

build-banner:
	@mkdir -p $(BUILD)
	@if [ -f $(CURDIR)/assets/banner/banner_model.cgfx ]; then \
		    echo "3D banner: using CGFX model assets/banner/banner_model.cgfx"; \
		    $(CURDIR)/tools/bannertool makecwav -i $(CURDIR)/assets/pk_sample.wav -o /tmp/banner_adpcm.cwav; \
		    $(CURDIR)/tools/bannertool makebanner -ci $(CURDIR)/assets/banner/banner_model.cgfx -ca /tmp/banner_adpcm.cwav -o $(BUILD)/banner.bin; \
	elif command -v python3 >/dev/null 2>&1 && python3 -c "from PIL import Image" 2>/dev/null; then \
		    python3 -c "\
from PIL import Image; \
	banner = Image.new('RGBA', (256, 128), (0, 0, 0, 0)); \
	logo = Image.open('$(CURDIR)/romfs/assets/logo/logo.png'); \
	w, h = logo.size; \
	new_w = 256; \
	new_h = int(h * new_w / w); \
	logo = logo.resize((new_w, new_h), Image.Resampling.LANCZOS); \
	x, y = (256 - logo.width) // 2, (128 - logo.height) // 2; \
	banner.paste(logo, (x, y), logo); \
banner.save('/tmp/banner_centered.png'); \
"; \
		    $(CURDIR)/tools/bannertool makecwav -i $(CURDIR)/assets/pk_sample.wav -o /tmp/banner_adpcm.cwav; \
		    $(CURDIR)/tools/bannertool makebanner -i /tmp/banner_centered.png -ca /tmp/banner_adpcm.cwav -o $(BUILD)/banner.bin; \
		elif command -v sips >/dev/null 2>&1; then \
			echo "WARNING: Pillow not available. Install with: pip3 install Pillow"; \
			echo "Using fallback sips (may stretch image)..."; \
			sips -z 128 256 $(CURDIR)/romfs/assets/logo/logo.png --out /tmp/banner_256x128.png >/dev/null 2>&1; \
			$(CURDIR)/tools/bannertool makebanner -i /tmp/banner_256x128.png -a $(CURDIR)/assets/pk_sample.wav -o $(BUILD)/banner.bin; \
		else \
			echo "WARNING: No banner.bin found and sips not available. Using empty banner."; \
			dd if=/dev/zero of=$(BUILD)/banner.bin bs=1 count=32 2>/dev/null; \
		fi

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).3dsx $(OUTPUT).smdh $(TARGET).elf


#---------------------------------------------------------------------------------
else

DEPENDS	:=	$(OFILES:.o=.d)

#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
ifeq ($(strip $(NO_SMDH)),)
$(OUTPUT).3dsx	:	$(OUTPUT).elf $(OUTPUT).smdh
else
$(OUTPUT).3dsx	:	$(OUTPUT).elf
endif

$(OUTPUT).elf	:	$(OFILES)

#---------------------------------------------------------------------------------
# you need a rule like this for each extension you use as binary data
#---------------------------------------------------------------------------------
%.bin.o	:	%.bin
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@$(bin2o)

#---------------------------------------------------------------------------------
define shader-as
	$(eval CURBIN := $(patsubst %.shbin.o,%.shbin,$(notdir $@)))
	picasso -o $(CURBIN) $1
	bin2s $(CURBIN) | $(AS) -o $@
	echo "extern const u8" `(echo $(CURBIN) | sed -e 's/^\([0-9]\)/_\1/' | tr . _)`"_end[];" > `(echo $(CURBIN) | tr . _)`.h
	echo "extern const u8" `(echo $(CURBIN) | sed -e 's/^\([0-9]\)/_\1/' | tr . _)`"[];" >> `(echo $(CURBIN) | tr . _)`.h
	echo "extern const u32" `(echo $(CURBIN) | sed -e 's/^\([0-9]\)/_\1/' | tr . _)`_size";" >> `(echo $(CURBIN) | tr . _)`.h
endef

%.shbin.o : %.v.pica %.g.pica
	@echo $(notdir $^)
	@$(call shader-as,$^)

%.shbin.o : %.v.pica
	@echo $(notdir $<)
	@$(call shader-as,$<)

%.shbin.o : %.shlist
	@echo $(notdir $<)
	@$(call shader-as,$(foreach file,$(shell cat $<),$(dir $<)$(file)))

-include $(DEPENDS)

#---------------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------------
