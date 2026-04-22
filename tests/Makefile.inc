# tests/Makefile.inc - Included by root Makefile
# Defines test targets with proper dependencies on $(TARGETS)
#
# This ensures yosys is built before running any tests.
# Usage: make test-aiger, make test-arch/ice40, etc.

# Test directories (paths relative to root)
MK_TEST_DIRS =
MK_TEST_DIRS += tests/arch/analogdevices
MK_TEST_DIRS += tests/arch/anlogic
MK_TEST_DIRS += tests/arch/ecp5
MK_TEST_DIRS += tests/arch/efinix
MK_TEST_DIRS += tests/arch/fabulous
MK_TEST_DIRS += tests/arch/gatemate
MK_TEST_DIRS += tests/arch/gowin
MK_TEST_DIRS += tests/arch/ice40
MK_TEST_DIRS += tests/arch/intel_alm
MK_TEST_DIRS += tests/arch/machxo2
MK_TEST_DIRS += tests/arch/microchip
MK_TEST_DIRS += tests/arch/nanoxplore
MK_TEST_DIRS += tests/arch/nexus
MK_TEST_DIRS += tests/arch/quicklogic/pp3
MK_TEST_DIRS += tests/arch/quicklogic/qlf_k6n10f
MK_TEST_DIRS += tests/arch/xilinx
MK_TEST_DIRS += tests/bind
MK_TEST_DIRS += tests/bugpoint
MK_TEST_DIRS += tests/opt
MK_TEST_DIRS += tests/sat
MK_TEST_DIRS += tests/sdc
MK_TEST_DIRS += tests/sim
MK_TEST_DIRS += tests/svtypes
MK_TEST_DIRS += tests/techmap
MK_TEST_DIRS += tests/various
MK_TEST_DIRS += tests/rtlil
ifeq ($(ENABLE_VERIFIC),1)
ifneq ($(YOSYS_NOVERIFIC),1)
MK_TEST_DIRS += tests/verific
endif
endif
MK_TEST_DIRS += tests/verilog
MK_TEST_DIRS += tests/arith_tree
MK_TEST_DIRS += tests/simple
MK_TEST_DIRS += tests/simple_abc9
MK_TEST_DIRS += tests/hana
MK_TEST_DIRS += tests/asicworld
MK_TEST_DIRS += tests/realmath
MK_TEST_DIRS += tests/share
MK_TEST_DIRS += tests/opt_share
MK_TEST_DIRS += tests/fsm
MK_TEST_DIRS += tests/memlib
MK_TEST_DIRS += tests/bram
MK_TEST_DIRS += tests/svinterfaces
MK_TEST_DIRS += tests/xprop
MK_TEST_DIRS += tests/select
MK_TEST_DIRS += tests/peepopt
MK_TEST_DIRS += tests/proc
MK_TEST_DIRS += tests/blif
MK_TEST_DIRS += tests/arch
MK_TEST_DIRS += tests/rpc
MK_TEST_DIRS += tests/memfile
MK_TEST_DIRS += tests/fmt
MK_TEST_DIRS += tests/cxxrtl
MK_TEST_DIRS += tests/liberty
MK_TEST_DIRS += tests/memories
MK_TEST_DIRS += tests/aiger
MK_TEST_DIRS += tests/alumacc

# Generate test-<dir> targets for each test directory
# These depend on $(TARGETS) ensuring yosys is built first
# Example: make test-aiger -> builds yosys, then runs aiger tests
define MK_TEST_TARGET
.PHONY: test-$(patsubst tests/%,%,$(1))
test-$(patsubst tests/%,%,$(1)): $$(TARGETS) $$(EXTRA_TARGETS)
	@$$(MAKE) -C tests $(patsubst tests/%,%,$(1))
endef

$(foreach dir,$(MK_TEST_DIRS),$(eval $(call MK_TEST_TARGET,$(dir))))

# Convenience target: test-arch runs all architecture tests
.PHONY: test-arch
test-arch: $(patsubst tests/%,test-%,$(filter tests/arch/%,$(MK_TEST_DIRS)))
