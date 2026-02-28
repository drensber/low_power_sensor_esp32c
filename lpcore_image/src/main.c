/*
 * Copyright (c) 2025 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <stdio.h>
#include <ulp_lp_core.h>
#include <ulp_lp_core_utils.h>
// #include <ulp_lp_core_i2c.h> // Uncomment later when doing Phase 3

void main(void)
{
    int i=0;
    while (1) {
	printf("LP core loop %d\n", ++i);
        // 1. Wake up the HP Core
        ulp_lp_core_wakeup_main_processor();
	
        // 2. Sleep for 5 seconds (Throttle the wakeups to measure power)
        ulp_lp_core_delay_us(5000000);
    }
    return 0;
}
