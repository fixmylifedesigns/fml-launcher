#---------------------------------------------------------------------------------
# CubeLaunch - small GameCube DOL launcher (built with devkitPPC + libogc)
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITPPC)),)
$(error "Please set DEVKITPPC in your environment. export DEVKITPPC=<path to>devkitPPC")
endif

include $(DEVKITPPC)/gamecube_rules

TARGET		:=	cubelaunch
BUILD		:=	build
SOURCES		:=	source
DATA		:=
INCLUDES	:=	source

CFLAGS		=	-O2 -Wall $(MACHDEP) $(INCLUDE)
ifneq ($(DEMO),)
CFLAGS		+=	-DSTART_DEMO
# The demo build carries the boot video, a song and a video itself, since Dolphin has no SD card
DATA		:=	data
endif
CXXFLAGS	=	$(CFLAGS)
ASFLAGS		=	$(MACHDEP)
LDFLAGS		=	$(MACHDEP) -Wl,-Map,$(notdir $@).map

LIBS		:=	-lfat -lmad -lasnd -logc -lm
LIBDIRS		:=

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) 			$(foreach dir,$(DATA),$(CURDIR)/$(dir))
export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.S)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.fmv) $(wildcard $(dir)/*.fpg) $(wildcard $(dir)/*.mp3) $(wildcard $(dir)/*.mpg)))

export LD	:=	$(CC)
export OFILES	:=	$(addsuffix .o,$(BINFILES)) $(CFILES:.c=.o) $(SFILES:.S=.o)

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-iquote $(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(CURDIR)/$(BUILD) -I$(LIBOGC_INC)

export LIBPATHS	:=	-L$(LIBOGC_LIB) $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

.PHONY: $(BUILD) clean

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(OUTPUT).elf $(OUTPUT).dol

else

DEPENDS	:=	$(OFILES:.o=.d)

$(OUTPUT).dol: $(OUTPUT).elf
$(OUTPUT).elf: $(OFILES)

-include $(DEPENDS)

%.fmv.o %_fmv.h : %.fmv
	@echo $(notdir $<)
	@$(bin2o)

%.fpg.o %_fpg.h : %.fpg
	@echo $(notdir $<)
	@$(bin2o)

%.mp3.o %_mp3.h : %.mp3
	@echo $(notdir $<)
	@$(bin2o)

%.mpg.o %_mpg.h : %.mpg
	@echo $(notdir $<)
	@$(bin2o)

endif
