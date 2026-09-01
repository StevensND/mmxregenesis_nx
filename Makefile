#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

#---------------------------------------------------------------------------------
TARGET		:=	mmxregenesis_nx
APP_TITLE	:=	Mega Man X Regenesis
APP_AUTHOR	:=	StevensND
APP_VERSION	:=	0.1-wip
BUILD		:=	build
SOURCES		:=	source
DATA		:=	data
INCLUDES	:=	source

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
ARCH	:=	-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

CFLAGS	:=	-g -Wall -O2 -ffunction-sections \
			$(ARCH) $(DEFINES)

CFLAGS	+=	$(INCLUDE) -D__SWITCH__ -DSHIM_CALL_ONCE=1 -fno-stack-protector

CXXFLAGS	:= $(CFLAGS)

ASFLAGS	:=	-g $(ARCH)
# libGLESv1_CM and libGLESv2 (mesa) both define the common GL entry points
# (glClear, glTexImage2D, ...) -- they are thin wrappers over the same libglapi
# dispatch, so let the linker take the first definition and still pull the
# GLES1-only (glOrthof, ...) and GLES2-only (glCreateShader, ...) symbols.
# FIXED build-id (not sha1): the on-disk shader cache is keyed by the module
# build-id. With sha1 it changes on every build, so each new binary starts cold
# and recompiles every shader. A constant value makes the cache persist across
# builds and restarts. Bump it if the shaders change.
LDFLAGS	=	-specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) \
			-Wl,--allow-multiple-definition -Wl,-Map,$(notdir $*.map) \
			-Wl,--build-id=0x4d4d586e78436163686556310000000000000001

# libgodot_android.so renders with GLES2/3 and imports its GL/EGL by name; we
# resolve those to the Mesa SDK through the import table. The wrapper owns the
# EGL context (the game does not create one). libEGL.a is fat, libGLESv2/libglapi
# are thin dispatch, so link the whole set in ONE rescan group. -lstdc++ backs
# mesa's C++; the game keeps its own libc++_shared.
LIBS	:= -pthread -Wl,-u,vk_icdGetInstanceProcAddr -Wl,-u,vk_icdNegotiateLoaderICDInterfaceVersion -Wl,--wrap=thrd_create -Wl,--wrap=pthread_create -Wl,--start-group \
			-l:libvulkan.a -l:libGLESv2.a -l:libEGL.a -l:libglapi.a \
			-l:libcompiler.a -l:libmesa_util_c11.a -l:libblake3.a -l:libmesa_util.a \
			-l:libmesa_util_simd.a -l:libxmlconfig.a \
			-lexpat -lz -lnx -lstdc++ -lm \
			-Wl,--end-group

#---------------------------------------------------------------------------------
# list of directories containing libraries, this must be the top level containing
# include and lib
#---------------------------------------------------------------------------------
# The Mesa SDK (CI extracts it into ./mesa-sdk) goes FIRST so its libEGL/
# libGLESv2/libglapi win over the stock switch-mesa portlib. Its on-disk shader
# cache persists across launches -> shaders compile once and the per-launch
# recompilation stalls go away.
MESA_SDK := $(CURDIR)/mesa-sdk/opt/devkitpro/portlibs/switch
LIBDIRS	:= $(MESA_SDK) $(PORTLIBS) $(LIBNX)

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
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

#---------------------------------------------------------------------------------
# use CXX for linking C++ projects, CC for standard C
#---------------------------------------------------------------------------------
export LD	:=	$(CXX)

export OFILES_BIN	:=	$(addsuffix .o,$(BINFILES))
export OFILES_SRC	:=	$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES 	:=	$(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN	:=	$(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

ifeq ($(strip $(ICON)),)
	icons := $(wildcard *.jpg)
	ifneq (,$(findstring $(TARGET).jpg,$(icons)))
		export APP_ICON := $(TOPDIR)/$(TARGET).jpg
	else
		ifneq (,$(findstring icon.jpg,$(icons)))
			export APP_ICON := $(TOPDIR)/icon.jpg
		endif
	endif
else
	export APP_ICON := $(TOPDIR)/$(ICON)
endif

ifeq ($(strip $(NO_ICON)),)
	ifneq ($(strip $(APP_ICON)),)
		export NROFLAGS += --icon=$(APP_ICON)
	endif
endif

ifeq ($(strip $(NO_NACP)),)
	export NROFLAGS += --nacp=$(CURDIR)/$(TARGET).nacp
endif

ifneq ($(APP_TITLEID),)
	export NACPFLAGS += --titleid=$(APP_TITLEID)
endif

ifneq ($(ROMFS),)
	export NROFLAGS += --romfsdir=$(CURDIR)/$(ROMFS)
endif

.PHONY: $(BUILD) clean all

#---------------------------------------------------------------------------------
all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf

#---------------------------------------------------------------------------------
else
.PHONY:	all

DEPENDS	:=	$(OFILES:.o=.d)

#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
all	:	$(OUTPUT).nro

$(OUTPUT).nro	:	$(OUTPUT).elf $(OUTPUT).nacp
$(OUTPUT).elf	:	$(OFILES)

$(OFILES_SRC)	: $(HFILES_BIN)

#---------------------------------------------------------------------------------
# you need a rule like this for each extension you use as binary data
#---------------------------------------------------------------------------------
%.bin.o	%_bin.h :	%.bin
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

#---------------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------------
