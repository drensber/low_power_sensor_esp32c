# SPDX-License-Identifier: Apache-2.0
#
# Copyright 2024 Espressif

# Add external project
ExternalZephyrProject_Add(
    APPLICATION low_power_sensor_lpcore
    SOURCE_DIR ${APP_DIR}/lpcore_image
    BOARD ${SB_CONFIG_ULP_REMOTE_BOARD}
  )

# Add dependencies so that the lpcore_image will be built first
# This is required because some primary cores need information from the
# lpcore_image's build, such as the output image's LMA
add_dependencies(low_power_sensor low_power_sensor_lpcore)
sysbuild_add_dependencies(CONFIGURE low_power_sensor low_power_sensor_lpcore)
sysbuild_add_dependencies(FLASH low_power_sensor_lpcore low_power_sensor)

if(SB_CONFIG_BOOTLOADER_MCUBOOT)
  # Make sure MCUboot is flashed first
  sysbuild_add_dependencies(FLASH low_power_sensor mcuboot)
endif()
