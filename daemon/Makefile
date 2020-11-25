#
# Copyright(c) 2020 Intel Corporation
# SPDX-License-Identifier: BSD-3-Clause-Clear
#

PWD:=$(shell pwd)
MODULESDIR:=$(PWD)/../modules

CGO_CFLAGS = -I$(MODULESDIR)/include/

CGO_FLAGS = CC="$(CC)" CXX="$(CXX)"
ifneq ($(CGO_CFLAGS),)
	CGO_FLAGS += CGO_CFLAGS="$(CGO_CFLAGS)"
endif
ifneq ($(CGO_LDFLAGS),)
	CGO_FLAGS += CGO_LDFLAGS="$(CGO_LDFLAGS)"
endif

all: sync
	$(MAKE) build

build:
	$(CGO_FLAGS) go build -o bin/main main.go

sync:
	@cd $(MODULESDIR) && $(MAKE) sync

clean:
	rm -rf bin/
