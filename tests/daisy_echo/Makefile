# firmware for a Daisy Seed, for SerialPortTests --daisy, see tests/README.md
#   make
#   make program-dfu                         the STM32 ROM bootloader (hold BOOT, press RESET)
#   make APP_TYPE=BOOT_SRAM program-dfu      the Daisy bootloader
TARGET = daisy_echo

CPP_SOURCES = daisy_echo.cpp

# a built libDaisy, by default the module's submodule
LIBDAISY_DIR ?= ../../submodules/libDaisy

# the CDC class, to see the requests the host sends (libDaisy 8 moved it to Middlewares/Patched)
C_INCLUDES = \
-I$(LIBDAISY_DIR)/src/usbd \
-I$(LIBDAISY_DIR)/Middlewares/ST/STM32_USB_Device_Library/Core/Inc \
-I$(LIBDAISY_DIR)/Middlewares/ST/STM32_USB_Device_Library/Class/CDC/Inc \
-I$(LIBDAISY_DIR)/Middlewares/Patched/ST/STM32_USB_Device_Library/Core/Inc \
-I$(LIBDAISY_DIR)/Middlewares/Patched/ST/STM32_USB_Device_Library/Class/CDC/Inc

SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile
