/*
 * main() routine for the low power core image.
 * 
 * Copyright (c) 2025 Dave Rensberger
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

#include "shared_data.h"

extern void read_aht20(lp_shared_data_t *);

static struct mbox_msg msg = {0};
static RTC_DATA_ATTR lp_shared_data_t mbox_message = {0};

const struct mbox_dt_spec tx_channel = MBOX_DT_SPEC_GET(DT_PATH(mbox_consumer), tx);

int main(void)
{

    int max_transfer_size_bytes = mbox_mtu_get_dt(&tx_channel);
    int mbox_message_size = sizeof(mbox_message);
    
    if (mbox_message_size > max_transfer_size_bytes) {
	printf("Size of mbox_message is %d bytes (max is %d)\n",
	       mbox_message_size, max_transfer_size_bytes);
    }
    
    printf("LP core loop %d\n", mbox_message.lp_wake_count);

    mbox_message.lp_wake_count++;

    read_aht20(&mbox_message);    

    // 1. Wake up the HP Core
#ifndef CONFIG_LPS_HPCORE_ALWAYS_STAY_AWAKE
    ulp_lp_core_wakeup_main_processor();
    mbox_message.hp_wake_count++;
#endif //  CONFIG_LPS_HPCORE_ALWAYS_STAY_AWAKE
    k_msleep(1000);

    msg.data = &mbox_message;
    msg.size = mbox_message_size;
    
    printf("Calling mbox_send with:\n"
	   "  .lp_wake_count=%d\n"
	   "  .hp_wake_count=%d\n"
	   "  .temp_c_x10=%d\n"
	   "  .rh_x10=%d\n",
	   ((lp_shared_data_t *) msg.data)->lp_wake_count,
	   ((lp_shared_data_t *) msg.data)->hp_wake_count,	   
	   ((lp_shared_data_t *) msg.data)->temp_c_x10,
	   ((lp_shared_data_t *) msg.data)->rh_x10);

    if (mbox_send_dt(&tx_channel, &msg) < 0) {
	    printf("mbox_send() error\n");
	}

    k_msleep(100);

    printf("Going to sleep\n");
    
    ulp_lp_core_memory_shared_cfg_t *cfg
	= ulp_lp_core_memory_shared_cfg_get();
    
    if (cfg->sleep_duration_ticks) {
	ulp_lp_core_lp_timer_set_wakeup_ticks
	    (cfg->sleep_duration_ticks);
    }
	
    //Busy wait to let the printf UART finish
    ulp_lp_core_delay_us(500);
    
    ulp_lp_core_halt();

    return 0;
}
