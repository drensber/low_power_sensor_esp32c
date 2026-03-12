/*
 * Copyright (c) 2025 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <stdio.h>
#include "ulp_lp_core.h"
#include "ulp_lp_core_utils.h"
#include "ulp_lp_core_memory_shared.h"
#include "ulp_lp_core_lp_timer_shared.h"
#include "esp_attr.h"
#include <zephyr/kernel.h>
#include <zephyr/drivers/mbox.h>
// #include "ulp_lp_core_i2c.h" // Uncomment later when doing Phase 3


static RTC_DATA_ATTR uint32_t lp_wake_count;
static RTC_DATA_ATTR struct mbox_msg msg = {0};

const RTC_DATA_ATTR struct mbox_dt_spec tx_channel = MBOX_DT_SPEC_GET(DT_PATH(mbox_consumer), tx);


int main(void)
{

    uint32_t mbox_message = 3041973;

    //int max_transfer_size_bytes = mbox_mtu_get_dt(&tx_channel);
    int max_transfer_size_bytes = 4;
    
    printf("LP core loop %d\n", lp_wake_count);
    // 1. Wake up the HP Core
    ulp_lp_core_wakeup_main_processor();

    ulp_lp_core_delay_us(5000000);

    msg.data = &mbox_message;
    msg.size = max_transfer_size_bytes;

    printf("Calling mbox_send with msg.data=%d, msg.size=%d\n",
	   *((uint32_t *) msg.data), msg.size);

    if (lp_wake_count != 0) {
	if (mbox_send_dt(&tx_channel, &msg) < 0) {
	    printf("mbox_send() error\n");
	}
    }
    
    lp_wake_count++;
    printf("Going to sleep\n");
    
    ulp_lp_core_memory_shared_cfg_t *cfg
	= ulp_lp_core_memory_shared_cfg_get();
    
    if (cfg->sleep_duration_ticks) {
	ulp_lp_core_lp_timer_set_wakeup_ticks
	    (cfg->sleep_duration_ticks);
    }
	
    //Busy wait to let the printf UART finish
    ulp_lp_core_delay_us(10000);
    
    ulp_lp_core_halt();

    return 0;
}
