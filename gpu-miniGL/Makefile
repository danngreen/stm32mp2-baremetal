BINARYNAME = main
BUILDDIR = build
SHAREDDIR = ../shared

SOURCES := startup.s
SOURCES += main.cc
SOURCES += psketch.cc
SOURCES += sketch.cc
# GPU library (etna API + 3D emitters) and mini-GL on top of it
SOURCES += ../gpu/etna.cc
SOURCES += ../gpu/etna_3d.cc
SOURCES += ../gpu/etna_compute.cc
SOURCES += ../gpu/pmic.cc
SOURCES += ../gpu/gpu_mmuv2.cc
SOURCES += ../gpu/gl/mini_gl.cc
SOURCES += ../gpu/gl/mini_gl_gpu.cc

# Display library: LVDS (EV1, default) or MIPI-DSI (custom devboard).
# `make BOARD=devboard` -> the ltdc-dsi/ ILI9881C path; otherwise the ltdc-lvds/
# path. Both provide the same display.hh / ltdc.hh API, so main.cc is agnostic
# (it just picks the matching panel_*.hh via -DDEVBOARD_0_1).
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
SOURCES += $(SHAREDDIR)/newlib/libcpp_stub.cc
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

EXTRA_ARCH_CFLAGS := -DUSE_HAL_DRIVER

OPTFLAG := -O2

FREESTANDING := 

include $(SHAREDDIR)/makefile-common.mk
