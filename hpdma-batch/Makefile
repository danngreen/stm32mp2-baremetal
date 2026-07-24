BINARYNAME = main
BUILDDIR = build
SHAREDDIR = ../shared

SOURCES := startup.s
SOURCES += main.cc
SOURCES += dma2d.cc

# Display library: LVDS (EV1, default) or MIPI-DSI (custom devboard) -- same
# selection as gpu-ltdc-demo. No GPU here: the CPU draws straight into the
# framebuffer, so this project only needs the display path + shared runtime.
BOARD ?= ev1
ifeq ($(BOARD),devboard)
  DISPLAYDIR := ../ltdc-dsi
  SOURCES += $(DISPLAYDIR)/dsi.cc
  SOURCES += $(DISPLAYDIR)/ili9881c.cc
  # ltdc-dsi drives the DSI + LTDC through ST's HAL drivers (hal.c provides the
  # HAL_Delay/tick base they use; hal_cnt.cc backs HAL_GetTick with the generic timer)
  SOURCES += $(SHAREDDIR)/STM32MP2xx_HAL_Driver/Src/stm32mp2xx_hal.c
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

INCLUDES := -I.
INCLUDES += -I$(DISPLAYDIR)
INCLUDES += -I$(SHAREDDIR)
INCLUDES += -I$(SHAREDDIR)/cmsis/Include
INCLUDES += -I$(SHAREDDIR)/cmsis/Core_A/Include
INCLUDES += -I$(SHAREDDIR)/cmsis-device/Include
INCLUDES += -I$(SHAREDDIR)/STM32MP2xx_HAL_Driver/Inc

EXTRA_ARCH_CFLAGS := -DUSE_HAL_DRIVER

OPTFLAG := -O2

include $(SHAREDDIR)/makefile-common.mk
