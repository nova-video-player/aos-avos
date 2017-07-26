# Extra flags:
#
#	VERBOSE=1 will show compiler and linker command lines
#

#VERBOSE=1
#DBG_ALLOC = ON

LOCAL_PATH := .

ifeq ($(ARCH),arm)
CROSS    = arm-linux-
TGT_BASE = arm
LIBDIR	 = lib
else
CROSS    =
TGT_BASE = sim
libdir.x86_64 = lib64
libdir.i686   = lib
MACHINE := $(shell uname -m)
LIBDIR = $(libdir.$(MACHINE))
endif

include $(LOCAL_PATH)/standalone.mk
include $(CONFIGMAKEFILE)

SRC_PATH = $(shell pwd)
TGT_PATH = $(BUILD_DIR)
TGT_NAME = avos
LIB_NAME = libavos.so

ifeq (1,$(VERBOSE))
quiet =
else
quiet = quiet_
endif

# more private stuff
-include priv.mk

ifeq (1,$(ASAN))
CC   = $(CROSS)clang
LD   = $(CROSS)clang
ASM  = $(CROSS)clang
LINK = $(CROSS)clang
else
CC   = $(CROSS)gcc 
LD   = $(CROSS)ld 
ASM  = $(CROSS)gcc
LINK = $(CROSS)gcc
endif
AR   = $(CROSS)ar
STRIP= $(CROSS)strip
OBJCOPY= $(CROSS)objcopy
MD5SUM = md5sum

CCFLAVOR = $(shell $(CC) --version | grep -oE -m 1 '(^gcc|clang)')

BUILD_FROM := ARCBUILD

all: avos-compile

include common.mk

CFLAGS += -g -O2
ifeq ($(ARCH),i586)
CFLAGS += -fPIC
endif
CFLAGS += $(DEFINES)

INCLUDES += -I$(TOOLCHAIN_PATH)/include/
INCLUDES += -I$(TOOLCHAIN_PATH)/usr/include/
INCLUDES += -I$(TOOLCHAIN_PATH)/usr/include/freetype2

SHARED_LIBS += -lzip -pthread -lm

LDFLAGS += -rdynamic
LDFLAGS += -Wl,-Map,$(TGT_NAME).map

LDFLAGS += -L$(TOOLCHAIN_PATH)/usr/$(LIBDIR)
LDFLAGS += -Wl,-rpath-link=$(TOOLCHAIN_PATH)/usr/$(LIBDIR)

LDFLAGS += -L$(TOOLCHAIN_PATH)/$(LIBDIR)
LDFLAGS += -Wl,-rpath-link=$(TOOLCHAIN_PATH)/$(LIBDIR)

LDFLAGS += $(AVOS_SHARED_LIBS) $(SHARED_LIBS)

# path to sources
VPATH = .:Source

DEPDIR = .deps

MAKEDEPEND = $(CC) -MM $(INCLUDES) $(EXTRA_CFLAGS) $(CFLAGS) -MT $@ -o $(TGT_PATH)/$(DEPDIR)/$*.dep $<

.SUFFIXES: .o .c .s .S .dep
#               *Implicit Rules*

      CMD_CC_O_C =  $(CC) $(INCLUDES) $(EXTRA_CFLAGS) $(CFLAGS) -o $@ -c $<
quiet_CMD_CC_O_C = "CC  $<"

      CMD_MKDEP = $(CC) -MM $(INCLUDES) $(EXTRA_CFLAGS)  $(CFLAGS) -MT $(TGT_PATH)/$*.o -o $(TGT_PATH)/$(DEPDIR)/$*.dep $<
quiet_CMD_MKDEP = "DEP $<"

      CMD_ASM_O_S = $(ASM) $(ASMFLAGS) -o $@ -c $<
quiet_CMD_ASM_O_S =  "AS  $<"

      CMD_LDSO = $(LINK) -shared -o $@ $^
quiet_CMD_LDSO = "LD  $@"

      CMD_AR = $(AR) rcs $@ $^
quiet_CMD_AR = "AR  `basename $@`"

ifeq ($(ARCH),arm)
$(TGT_PATH)/gfx_rgba32.o  : CFLAGS += -mfloat-abi=softfp -mfpu=neon
$(TGT_PATH)/$(DEPDIR)/gfx_rgba32.dep  : CFLAGS += -mfloat-abi=softfp -mfpu=neon
endif

$(TGT_PATH)/%.o : %.c
	@echo $($(quiet)CMD_CC_O_C)
	@$(CMD_CC_O_C)

$(TGT_PATH)/%.o : %.S
	@$(MAKEDEPEND); 
	@echo $($(quiet)CMD_ASM_O_S)
	@$(CMD_ASM_O_S)

$(TGT_PATH)/$(DEPDIR)/%.dep: %.c
	@mkdir -p $(TGT_PATH)/$(DEPDIR)
	@echo $($(quiet)CMD_MKDEP)
	@$(CMD_MKDEP)

# Dependencies

LIB_OBJS = $(sort $(patsubst %,$(TGT_PATH)/%,$(CSRC:.c=.o)))     $(patsubst %,$(TGT_PATH)/%,$(XASRC:.S=.o)) 
APP_OBJS = $(sort $(patsubst %,$(TGT_PATH)/%,$(CSRC_APP:.c=.o))) $(patsubst %,$(TGT_PATH)/%,$(XASRC_APP:.S=.o)) 

ifeq (,$(NO_MAKEFILE_DEPS))
$(LIB_OBJS): Makefile common.mk $(SUBMAKEFILES) $(CONFIGMAKEFILE)
$(APP_OBJS): Makefile common.mk $(SUBMAKEFILES) $(CONFIGMAKEFILE)
endif

.KEEP_STATE:

# *Explicit Rules*

ifeq (1,$(ASAN))
ASAN_FLAGS = -g -fsanitize=address
endif

CMD_LIB = $(LINK) $(ASAN_FLAGS) -shared -o $(TGT_PATH)/$(LIB_NAME) $(LIB_OBJS)
quiet_CMD_LIB = "LIB $(LIB_NAME)"

ifeq (YES,$(LIBAVOS))
# build libavos.so:
LIB_DEPS  += $(TGT_PATH)/$(LIB_NAME)
LDFLAGS += -lavos $(LIB_RPATH)
else
# build one big binary
APP_OBJS += $(LIB_OBJS)
endif

CMD_LINK = $(LINK) $(LDFLAGS) -o $(TGT_PATH)/$(TGT_NAME) $(APP_OBJS) $(STATIC_LIBS) -L$(TGT_PATH)
quiet_CMD_LINK = "LD  $(TGT_NAME)"

$(TGT_PATH)/$(LIB_NAME): $(LIB_OBJS)
	@install -d $(TGT_PATH)/
	@echo $($(quiet)CMD_LIB)
	@$(CMD_LIB) 

$(TGT_PATH)/$(TGT_NAME): $(APP_OBJS) $(LIB_DEPS) $(STATIC_LIBS)
	@install -d $(TGT_PATH)/
	@echo $($(quiet)CMD_LINK)
	@$(CMD_LINK) 

$(TGT_PATH)/avsh: avsh_client.c
	@echo "CC  Source/ahsv_client.c"
	@$(CC) -g -o $(TGT_PATH)/avsh Source/avsh_client.c

ifeq ($(BUILD),DEBUG)
avsh_install: $(TGT_PATH)/avsh
	@rm -f $(INSTALL_DIR)/usr/bin/avsh
	install -D -m 755 $(TGT_PATH)/avsh $(INSTALL_DIR)/usr/bin/avsh
else
avsh_install:
endif
	
# install unconditionaly avos in root fs
# there might be other binaries to install (bitmaps, anyone ?)
avos-install: avos-compile avsh_install 
	@rm -f $(INSTALL_DIR)/usr/bin/avos
	install -D -m 755 $(TGT_PATH)/$(TGT_NAME) $(INSTALL_DIR)/usr/bin/avos
ifeq ($(GUI),ON)
	install -D -m 644 rgb_bitmaps_wide_vga.zip $(INSTALL_DIR)/usr/share/fonts/bitmaps.zip
endif
#ifeq ($(BUILD),RELEASE)
	# strip only on release builds so that we can gdb
	# forget that, avos is 14MB! - if you need to gdb, then do it from /mnt/data!
	$(STRIP) -S $(INSTALL_DIR)/usr/bin/avos
#endif

avos-compile: $(AVOS_DEPS) $(TGT_PATH)/$(TGT_NAME) $(TGT_PATH)/avsh

clean:
	@rm -rf $(TGT_PATH)/$(DEPDIR) $(TGT_PATH)/*

cleaner: clean $(AVOS_CLEAN_DEPS)

test:
	make -C test
	
# include deps depending on target
ifneq (,$(TGT_PATH))
ifeq (,$(NO_DEPS))
-include $(sort $(CSRC:%.c=$(TGT_PATH)/$(DEPDIR)/%.dep))
-include $(sort $(CSRC_APP:%.c=$(TGT_PATH)/$(DEPDIR)/%.dep))
endif
endif

.PHONY : test $(PHONY_TARGETS)

FORCE: # must remain empty !
