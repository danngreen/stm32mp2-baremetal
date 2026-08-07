BINARYNAME = main
BUILDDIR = build
SHAREDDIR = ../shared

# Which Processing sketch to build, as a .pde path relative to this directory:
#   make BOARD=devboard DEMO=sketches/Basics/color/Radial_Gradient.pde
DEMO ?= sketches/Topics/Motion/BouncyBubbles.pde

SOURCES := startup.s
SOURCES += main.cc
SOURCES += psketch.cc
SOURCES += sketch_pde.cc
SOURCES += newlib_syscalls.cc
# GPU library (etna API + 3D emitters) and mini-GL on top of it
SOURCES += ../gpu/etna.cc
SOURCES += ../gpu/etna_3d.cc
SOURCES += ../gpu/etna_compute.cc
SOURCES += ../gpu/pmic.cc
SOURCES += ../gpu/gpu_mmuv2.cc
SOURCES += ../gpu/gl/mini_gl.cc
SOURCES += ../gpu/gl/mini_gl_gpu.cc

BOARD ?= ev1
ifeq ($(BOARD),devboard)
  DISPLAYDIR := ../ltdc-dsi
  SOURCES += $(DISPLAYDIR)/dsi.cc
  SOURCES += $(DISPLAYDIR)/ili9881c.cc
  # ltdc-dsi drives the DSI + LTDC through ST's HAL drivers
  SOURCES += $(SHAREDDIR)/STM32MP2xx_HAL_Driver/Src/stm32mp2xx_hal_dsi.c
  SOURCES += $(SHAREDDIR)/STM32MP2xx_HAL_Driver/Src/stm32mp2xx_hal_ltdc.c
else
  DISPLAYDIR := ../ltdc-lvds
  SOURCES += $(DISPLAYDIR)/lvds.cc
endif
SOURCES += $(DISPLAYDIR)/display.cc
SOURCES += $(DISPLAYDIR)/ltdc.cc
SOURCES += $(SHAREDDIR)/aarch64/vectors.S
SOURCES += $(SHAREDDIR)/mmu/mmu.cc
SOURCES += $(SHAREDDIR)/drivers/hal_cnt.cc
SOURCES += $(SHAREDDIR)/interrupt/irq_init.c
SOURCES += $(SHAREDDIR)/interrupt/interrupt_handler.cc
SOURCES += $(SHAREDDIR)/print/uart_print.c
SOURCES += $(SHAREDDIR)/print/print.cc
SOURCES += $(SHAREDDIR)/aarch64/esr_decode.cc
SOURCES += $(SHAREDDIR)/newlib/syscall_stubs.c
SOURCES += $(SHAREDDIR)/drivers/pin.cc
SOURCES += $(SHAREDDIR)/STM32MP2xx_HAL_Driver/Src/stm32mp2xx_hal.c
SOURCES += $(SHAREDDIR)/STM32MP2xx_HAL_Driver/Src/stm32mp2xx_hal_i2c.c
SOURCES += $(SHAREDDIR)/STM32MP2xx_HAL_Driver/Src/stm32mp2xx_hal_i2c_ex.c

INCLUDES := -I.
INCLUDES += -I../gpu
INCLUDES += -I$(DISPLAYDIR)
INCLUDES += -I$(SHAREDDIR)
INCLUDES += -I$(SHAREDDIR)/cmsis/Include
INCLUDES += -I$(SHAREDDIR)/cmsis/Core_A/Include
INCLUDES += -I$(SHAREDDIR)/cmsis-device/Include
INCLUDES += -I$(SHAREDDIR)/STM32MP2xx_HAL_Driver/Inc

INCLUDES += -I$(BUILDDIR) # for the generated pde_prototypes.hh

EXTRA_ARCH_CFLAGS := -DUSE_HAL_DRIVER
EXTRA_ARCH_CFLAGS += -DPSKETCH_DEMO=\"$(DEMO)\"

OPTFLAG := -O2

FREESTANDING :=
LINK_STDLIB :=

# Rebuild everything when the selected sketch changes
CONFIG_STAMP_VALS += $(subst /,-,$(DEMO))

include $(SHAREDDIR)/makefile-common.mk

# Forward declarations for the sketch's functions -- Java ignores definition
# order, C++ does not. Regenerated when the .pde or the generator changes, and
# (via the config stamp) when a different DEMO is selected.
$(BUILDDIR)/pde_prototypes.hh: $(DEMO) tools/pde_prototypes.py $(CONFIG_STAMP)
	@mkdir -p $(BUILDDIR)
	python3 tools/pde_prototypes.py $(DEMO) $@

$(OBJDIR)/sketch_pde.o: $(BUILDDIR)/pde_prototypes.hh
