#
# A Makefile for the scst-local ...
#

SHELL=/bin/bash

#SCST_INC_DIR := /usr/local/include/scst
#SCST_DIR := $(SCST_INC_DIR)
SCST_INC_DIR	:= $(SUBDIRS)/../scst/include/
SCST_DIR	:= $(shell pwd)/../scst/src
KMOD 		:= $(shell pwd)/kernel

enable-Wextra = $(shell uname_r="$$(uname -r)"; if [ "$${uname_r%.el5}" = "$${uname_r}" ]; then echo "$(1)"; fi)

EXTRA_CFLAGS	+= -I$(SCST_INC_DIR) -I$(SCST_DIR)
EXTRA_CFLAGS	+= $(call enable-Wextra,-Wextra -Wno-unused-parameter\
	-Wno-missing-field-initializers)

#EXTRA_CFLAGS += -DCONFIG_SCST_LOCAL_DIRECT_PROCESSING

EXTRA_CFLAGS += -DCONFIG_SCST_EXTRACHECKS

#EXTRA_CFLAGS += -DCONFIG_SCST_TRACING

EXTRA_CFLAGS += -DCONFIG_SCST_DEBUG -g -fno-inline -fno-inline-functions

ifeq ($(KVER),)
  ifeq ($(KDIR),)
    KDIR := /lib/modules/$(shell uname -r)/build
  endif
else
  KDIR := /lib/modules/$(KVER)/build
endif

ifneq ($(PATCHLEVEL),)
obj-m	:= scst_local.o
else

all: Modules.symvers Module.symvers
	$(MAKE) -C $(KDIR) SUBDIRS=$(shell pwd) BUILD_INI=m

install: all
	$(MAKE) -C $(KDIR) SUBDIRS=$(shell pwd) BUILD_INI=m \
		modules_install
	-/sbin/depmod -aq $(KVER)

SCST_MOD_VERS := $(shell ls $(SCST_DIR)/Modules.symvers 2>/dev/null)
ifneq ($(SCST_MOD_VERS),)
Modules.symvers: $(SCST_DIR)/Modules.symvers
	cp $(SCST_DIR)/Modules.symvers .
else
.PHONY: Modules.symvers
endif

# It's renamed in 2.6.18
SCST_MOD_VERS := $(shell ls $(SCST_DIR)/Module.symvers 2>/dev/null)
ifneq ($(SCST_MOD_VERS),)
Module.symvers: $(SCST_DIR)/Module.symvers
	cp $(SCST_DIR)/Module.symvers .
else
.PHONY: Module.symvers
endif

uninstall:
	rm -f $(INSTALL_DIR)/scst_local.ko
	-/sbin/depmod -a $(KVER)
endif

clean:
	@$(MAKE) -C $(KDIR) M=$(PWD) clean
	@$(RM) tags Modules.symvers module.symvers Module.markers modules.order

extraclean: clean
	rm -f *.orig *.rej

2release:
	sed -i.aa s/"^E\?XTRA_CFLAGS += \-DCONFIG_SCST_EXTRACHECKS"/"#EXTRA_CFLAGS += \-DCONFIG_SCST_EXTRACHECKS"/ Makefile
	grep "^#EXTRA_CFLAGS += \-DCONFIG_SCST_EXTRACHECKS" Makefile >/dev/null
	sed -i.aa s/"^#\?EXTRA_CFLAGS += \-DCONFIG_SCST_TRACING"/"EXTRA_CFLAGS += \-DCONFIG_SCST_TRACING"/ Makefile
	grep "^EXTRA_CFLAGS += \-DCONFIG_SCST_TRACING" Makefile >/dev/null
	sed -i.aa s/"^E\?XTRA_CFLAGS += \-DCONFIG_SCST_DEBUG -g -fno-inline -fno-inline-functions"/"#EXTRA_CFLAGS += \-DCONFIG_SCST_DEBUG -g -fno-inline -fno-inline-functions"/ Makefile
	grep "^#EXTRA_CFLAGS += \-DCONFIG_SCST_DEBUG -g -fno-inline -fno-inline-functions" Makefile >/dev/null
	rm Makefile.aa

2debug:
	sed -i.aa s/"^#\?EXTRA_CFLAGS += \-DCONFIG_SCST_EXTRACHECKS"/"EXTRA_CFLAGS += \-DCONFIG_SCST_EXTRACHECKS"/ Makefile
	grep "^EXTRA_CFLAGS += \-DCONFIG_SCST_EXTRACHECKS" Makefile >/dev/null
	sed -i.aa s/"^E\?XTRA_CFLAGS += \-DCONFIG_SCST_TRACING"/"#EXTRA_CFLAGS += \-DCONFIG_SCST_TRACING"/ Makefile
	grep "^#EXTRA_CFLAGS += \-DCONFIG_SCST_TRACING" Makefile >/dev/null
	sed -i.aa s/"^#\?EXTRA_CFLAGS += \-DCONFIG_SCST_DEBUG -g -fno-inline -fno-inline-functions"/"EXTRA_CFLAGS += \-DCONFIG_SCST_DEBUG -g -fno-inline -fno-inline-functions"/ Makefile
	grep "^EXTRA_CFLAGS += \-DCONFIG_SCST_DEBUG -g -fno-inline -fno-inline-functions" Makefile >/dev/null
	rm Makefile.aa

2perf:
	sed -i.aa s/"^E\?XTRA_CFLAGS += \-DCONFIG_SCST_EXTRACHECKS"/"#EXTRA_CFLAGS += \-DCONFIG_SCST_EXTRACHECKS"/ Makefile
	grep "^#EXTRA_CFLAGS += \-DCONFIG_SCST_EXTRACHECKS" Makefile >/dev/null
	sed -i.aa s/"^E\?XTRA_CFLAGS += \-DCONFIG_SCST_TRACING"/"#EXTRA_CFLAGS += \-DCONFIG_SCST_TRACING"/ Makefile
	grep "^#EXTRA_CFLAGS += \-DCONFIG_SCST_TRACING" Makefile >/dev/null
	sed -i.aa s/"^E\?XTRA_CFLAGS += \-DCONFIG_SCST_DEBUG -g -fno-inline -fno-inline-functions"/"#EXTRA_CFLAGS += \-DCONFIG_SCST_DEBUG -g -fno-inline -fno-inline-functions"/ Makefile
	grep "^#EXTRA_CFLAGS += \-DCONFIG_SCST_DEBUG -g -fno-inline -fno-inline-functions" Makefile >/dev/null
	rm Makefile.aa

.PHONY: all tgt install uninstall clean extraclean 2release 2debug 2perf

