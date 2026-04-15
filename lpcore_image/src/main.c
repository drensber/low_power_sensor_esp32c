/*
 * main() routine for the low power core image.
 * 
 * Copyright (c) 2025 Dave Rensberger
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "ulp_lp_core.h"
#include "ulp_lp_core_utils.h"
#include "ulp_lp_core_memory_shared.h"
#include "ulp_lp_core_lp_timer_shared.h"
#include "esp_attr.h"
#include <zephyr/kernel.h>
#include <zephyr/drivers/mbox.h>

#include "shared_data.h"

extern void read_aht20(lp_to_hp_shared_data_t *);

static struct mbox_msg msg = {0};
static RTC_DATA_ATTR lp_to_hp_shared_data_t mbox_message = {0};

static RTC_DATA_ATTR int16_t most_recent_published_temp = -100;
static RTC_DATA_ATTR uint16_t most_recent_published_hum;
static RTC_DATA_ATTR uint16_t wakeups_since_last_publish;

const struct mbox_dt_spec tx_channel = MBOX_DT_SPEC_GET(DT_PATH(mbox_consumer), tx);

int main(void)
{
    ulp_lp_core_memory_shared_cfg_t *cfg
	= ulp_lp_core_memory_shared_cfg_get();

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
    bool time_threshold_exceeded=true;
    bool temperature_threshold_exceeded=true;
    bool humidity_threshold_exceeded=true; 

#if defined(CONFIG_LPS_MAXIMUM_TIME_BETWEEN_PUBLISH_SECONDS) \
    && CONFIG_LPS_MAXIMUM_TIME_BETWEEN_PUBLISH_SECONDS != 0
    uint32_t seconds_since_last_publish =
	(wakeups_since_last_publish * (cfg->sleep_duration_us/1000000));
    printf("seconds_since_last_publish = %d\n", seconds_since_last_publish);
    
    if (seconds_since_last_publish < CONFIG_LPS_MAXIMUM_TIME_BETWEEN_PUBLISH_SECONDS) {
	time_threshold_exceeded=false;
    }
#endif 

#if defined(CONFIG_LPS_TEMP_CHANGE_PUBLISH_THRESHOLD) \
    && CONFIG_LPS_TEMP_CHANGE_PUBLISH_THRESHOLD != 0
    printf("temperature change is %d\n",
	   abs(mbox_message.temp_c_x10 - most_recent_published_temp));
    if (abs(mbox_message.temp_c_x10 - most_recent_published_temp) <
	CONFIG_LPS_TEMP_CHANGE_PUBLISH_THRESHOLD) {
	temperature_threshold_exceeded=false;
    }
#endif    

#if defined(CONFIG_LPS_HUM_CHANGE_PUBLISH_THRESHOLD) \
    && CONFIG_LPS_HUM_CHANGE_PUBLISH_THRESHOLD != 0
    printf("humidity change is %d\n",
	   abs(mbox_message.rh_x10 - most_recent_published_hum));
    if (abs(mbox_message.rh_x10 - most_recent_published_hum) <
	CONFIG_LPS_HUM_CHANGE_PUBLISH_THRESHOLD) {
	humidity_threshold_exceeded=false;
    }
#endif    
    
    if (time_threshold_exceeded ||
	temperature_threshold_exceeded ||
	humidity_threshold_exceeded) {
	
	ulp_lp_core_wakeup_main_processor();
	mbox_message.hp_wake_count++;

	// Give HP core time to boot to the point where it can recieve mbox messages
	k_msleep(1000);
	
	msg.data = &mbox_message;
	msg.size = mbox_message_size;
	
	printf("Calling mbox_send with:\n"
	       "  .lp_wake_count=%d\n"
	       "  .hp_wake_count=%d\n"
	       "  .temp_c_x10=%d\n"
	       "  .rh_x10=%d\n",
	       ((lp_to_hp_shared_data_t *) msg.data)->lp_wake_count,
	       ((lp_to_hp_shared_data_t *) msg.data)->hp_wake_count,	   
	       ((lp_to_hp_shared_data_t *) msg.data)->temp_c_x10,
	       ((lp_to_hp_shared_data_t *) msg.data)->rh_x10);
	
	if (mbox_send_dt(&tx_channel, &msg) < 0) {
	    printf("mbox_send() error\n");
	}
	
	k_msleep(100);

	
	// TODO: really needs to wait for mbox acknowledgement from HP core that it succeeded.
	wakeups_since_last_publish = 0;
	most_recent_published_hum = mbox_message.rh_x10;
	most_recent_published_temp = mbox_message.temp_c_x10;
    }
    else {
	wakeups_since_last_publish++;
    }

    
#endif //  not CONFIG_LPS_HPCORE_ALWAYS_STAY_AWAKE

    printf("LP core Going to sleep\n");
        
    if (cfg->sleep_duration_ticks) {
	ulp_lp_core_lp_timer_set_wakeup_ticks
	    (cfg->sleep_duration_ticks);
    }
	
    //Busy wait to let the printf UART finish
    ulp_lp_core_delay_us(500);
    
    ulp_lp_core_halt();

    return 0;
}
