# Copyright (c) 2025 Espressif Systems (Shanghai) Co., Ltd.
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

set(REMOTE_ZEPHYR_DIR ${CMAKE_CURRENT_BINARY_DIR}/../low_power_sensor_lpcore/zephyr)
set(LP_FULL_BIN ${CMAKE_BINARY_DIR}/../low_power_sensor_lpcore/zephyr/zephyr.bin)
set(LP_STRIPPED_BIN ${CMAKE_BINARY_DIR}/../low_power_sensor_lpcore/zephyr/zephyr_stripped.bin)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

message(STATUS "${CONFIG_BOARD_TARGET} compile as Master in this sample")
project(low_power_sensor_hpcore)


if(CONFIG_LPS_EXPLICIT_LP_IMAGE_LOADING)
    # This creates a recipe to produce 'zephyr_stripped.bin'
    add_custom_command(
        OUTPUT ${LP_STRIPPED_BIN}
        COMMAND head -c 8192 ${LP_FULL_BIN} > ${LP_STRIPPED_BIN}
        DEPENDS ${LP_FULL_BIN}
        COMMENT ">>> HP BUILD: Stripping LP binary to 8KB <<<"
    )

    # Create a Target to Trigger the Command
    add_custom_target(prepare_lp_binary DEPENDS ${LP_STRIPPED_BIN})

    # This ensures 'zephyr_stripped.bin' exists BEFORE 'src/main.c' or 'src/lp_firmware.S' are compiled.
    add_dependencies(app prepare_lp_binary)
endif()

target_sources(app PRIVATE src/main.c)

target_sources_ifdef(CONFIG_LPS_EXPLICIT_LP_IMAGE_LOADING app PRIVATE src/lp_firmware.S)
