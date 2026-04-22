# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2010-2016 Intel Corporation

# binary name
APP = l3fwd

# all source are stored in SRCS-y
SRCS-y := main.c l3fwd_acl.c l3fwd_lpm.c l3fwd_fib.c l3fwd_em.c l3fwd_event.c
SRCS-y += l3fwd_event_generic.c l3fwd_event_internal_port.c cnf_security_layer.c

PKGCONF ?= pkg-config

# Build using pkg-config variables if possible
ifneq ($(shell $(PKGCONF) --exists libdpdk && echo 0),0)
$(error "no installation of DPDK found")
endif

all: shared
.PHONY: shared static
shared: build/$(APP)-shared
	ln -sf $(APP)-shared build/$(APP)
static: build/$(APP)-static
	ln -sf $(APP)-static build/$(APP)

PC_FILE := $(shell $(PKGCONF) --path libdpdk 2>/dev/null)
CFLAGS += -O3 $(shell $(PKGCONF) --cflags libdpdk)
# Added for 'rte_eth_link_to_str()'
CFLAGS += -DALLOW_EXPERIMENTAL_API
LDFLAGS_SHARED = $(shell $(PKGCONF) --libs libdpdk)
LDFLAGS_STATIC = $(shell $(PKGCONF) --static --libs libdpdk)

ifeq ($(MAKECMDGOALS),static)
# check for broken pkg-config
ifeq ($(shell echo $(LDFLAGS_STATIC) | grep 'whole-archive.*l:lib.*no-whole-archive'),)
$(warning "pkg-config output list does not contain drivers between 'whole-archive'/'no-whole-archive' flags.")
$(error "Cannot generate statically-linked binaries with this version of pkg-config")
endif
endif

LDFLAGS += -lseccomp

build/$(APP)-shared: $(SRCS-y) Makefile $(PC_FILE) | build
	$(CC) $(CFLAGS) $(SRCS-y) -o $@ $(LDFLAGS) $(LDFLAGS_SHARED)

build/$(APP)-static: $(SRCS-y) Makefile $(PC_FILE) | build
	$(CC) $(CFLAGS) $(SRCS-y) -o $@ $(LDFLAGS) $(LDFLAGS_STATIC)

build:
	@mkdir -p $@

# ============================================================================
# Standalone targets (no DPDK required)
# ============================================================================

# Latency benchmark (standalone — no DPDK dependency)
.PHONY: benchmark
benchmark: latency_benchmark

latency_benchmark: latency_benchmark.c cnf_security_layer.c cnf_security_layer.h
	$(CC) -O3 -Wall -Wextra -o $@ latency_benchmark.c cnf_security_layer.c -lseccomp -lm

# Security tests (standalone — no DPDK dependency)
.PHONY: tests
tests: test_seccmp_graceful test_seccmp_trap

test_seccmp_graceful: test_seccmp_graceful.c cnf_security_layer.c cnf_security_layer.h
	$(CC) -O3 -Wall -Wextra -o $@ test_seccmp_graceful.c cnf_security_layer.c -lseccomp

test_seccmp_trap: test_seccmp_trap.c cnf_security_layer.c cnf_security_layer.h
	$(CC) -O3 -Wall -Wextra -o $@ test_seccmp_trap.c cnf_security_layer.c -lseccomp

# Build everything that can be built without DPDK
.PHONY: standalone
standalone: benchmark tests
	@echo ""
	@echo "========================================="
	@echo " Standalone builds complete:"
	@echo "   ./latency_benchmark [N] [output.csv]"
	@echo "   ./test_seccmp_graceful"
	@echo "   ./test_seccmp_trap"
	@echo "========================================="

.PHONY: clean
clean:
	rm -f build/$(APP) build/$(APP)-static build/$(APP)-shared
	rm -f latency_benchmark test_seccmp_graceful test_seccmp_trap
	rm -f latency_results.csv
	test -d build && rmdir -p build || true
