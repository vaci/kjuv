.PHONY = debug release debug-continuous release-continuous clean

SHELL := bash

export LIBS = \
  -lcapnpc -lcapnp-rpc -lcapnp \
  -lcapnp-json \
  -lkj-async -lkj-gzip -lkj-http -lkj-tls -lkj-test -lkj \
  -luv \
  -lkj-test \
  -lssl -lcrypto \
  -lpthread \
  -lz \
  -lgtest_main -lgtest

NIX_BUILD_CORES ?= 7
EKAM_REMAP_BYPASS_DIRS = $(HOME)/.cache/
EKAM_FLAGS := -j $(NIX_BUILD_CORES)

.DEFAULT: release

.PHONY: debug debug-continuous
.PHONY: release release-continuous

debug debug-continuous: export CXXFLAGS+=-Og -ggdb
release release-continuous: export CXXFLAGS+=-O2 -DNDEBUG

debug-continuous release-continuous: EKAM_FLAGS += -c

debug debug-continuous release release-continuous:
	nice ekam $(EKAM_FLAGS)

clean:
	rm -fr bin tmp

