/*
 * main() routine for the low power core image.
 * 
 * Copyright (c) 2026 Dave Rensberger
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/mbox.h>
#include <zephyr/logging/log.h>
#include "ulp_lp_core.h"
#include "ulp_lp_core_utils.h"
#include "ulp_lp_core_memory_shared.h"
#include "ulp_lp_core_lp_timer_shared.h"
#include "esp_attr.h"

#include "shared_data.h"

LOG_MODULE_REGISTER(lps_lp, CONFIG_LPS_LOG_LEVEL);

extern void read_aht20(lp_to_hp_shared_data_t *);

static struct mbox_msg msg = {0};
static RTC_DATA_ATTR lp_to_hp_shared_data_t mbox_message = {0};

static RTC_DATA_ATTR int16_t most_recent_published_temp = -100;
static RTC_DATA_ATTR uint16_t most_recent_published_hum;
static RTC_DATA_ATTR uint16_t wakeups_since_last_publish;

static RTC_DATA_ATTR volatile publish_status_t hp_publish_status = PUBLISH_STATUS_PENDING; 
static RTC_DATA_ATTR bool waiting_for_ack = false;

const struct mbox_dt_spec tx_channel = MBOX_DT_SPEC_GET(DT_PATH(mbox_consumer), tx);
const struct mbox_dt_spec rx_channel = MBOX_DT_SPEC_GET(DT_PATH(mbox_consumer), rx);

static void mbox_callback(const struct device *dev,
	        	     mbox_channel_id_t channel_id, 
                             void *user_data, struct mbox_msg *data) 
{
    LOG_DBG("LP core mbox_callback called");
 
}

int main(void)
{
    ulp_lp_core_memory_shared_cfg_t *cfg
	= ulp_lp_core_memory_shared_cfg_get();

    int max_transfer_size_bytes = mbox_mtu_get_dt(&tx_channel);
    int mbox_message_size = sizeof(mbox_message);

    mbox_register_callback_dt(&rx_channel, mbox_callback, NULL);
    mbox_set_enabled_dt(&rx_channel, 1);
    
    if (mbox_message_size > max_transfer_size_bytes) {
	LOG_DBG("Size of mbox_message is %d bytes (max is %d)",
	       mbox_message_size, max_transfer_size_bytes);
    }
    
    LOG_DBG("LP core loop %d", mbox_message.lp_wake_count);

    mbox_message.lp_wake_count++;

    read_aht20(&mbox_message);    

    LOG_DBG("hp_publish_status is %d", hp_publish_status);
    
    if (waiting_for_ack) {
        if (hp_publish_status == PUBLISH_STATUS_SUCCESS) {
            LOG_DBG("Previous publish SUCCEEDED.");
            waiting_for_ack = false;
            
            // Safe to reset thresholds now!
            wakeups_since_last_publish = 0;
            most_recent_published_hum = mbox_message.rh_x10;
            most_recent_published_temp = mbox_message.temp_c_x10;
            
        } else if (hp_publish_status == PUBLISH_STATUS_FAILURE) {
            LOG_DBG("Previous publish FAILED. Thresholds intact. Retrying.");
            waiting_for_ack = false;
            
        } else {
            // Still 0. The HP core must be taking a very long time to connect, 
            // or it crashed. We will wait one more cycle.
            LOG_DBG("Publish still pending...");
        }
    }    

    bool time_threshold_exceeded=true;
    bool temperature_threshold_exceeded=true;
    bool humidity_threshold_exceeded=true; 

#if defined(CONFIG_LPS_MAXIMUM_TIME_BETWEEN_PUBLISH_SECONDS) \
    && CONFIG_LPS_MAXIMUM_TIME_BETWEEN_PUBLISH_SECONDS != 0
    uint32_t seconds_since_last_publish =
	(wakeups_since_last_publish * (cfg->sleep_duration_us/1000000));
    LOG_DBG("seconds_since_last_publish = %d", seconds_since_last_publish);
    
    if (seconds_since_last_publish < CONFIG_LPS_MAXIMUM_TIME_BETWEEN_PUBLISH_SECONDS) {
	time_threshold_exceeded=false;
    }
#endif 

#if defined(CONFIG_LPS_TEMP_CHANGE_PUBLISH_THRESHOLD) \
    && CONFIG_LPS_TEMP_CHANGE_PUBLISH_THRESHOLD != 0
    LOG_DBG("temperature change is %d",
	   abs(mbox_message.temp_c_x10 - most_recent_published_temp));
    if (abs(mbox_message.temp_c_x10 - most_recent_published_temp) <
	CONFIG_LPS_TEMP_CHANGE_PUBLISH_THRESHOLD) {
	temperature_threshold_exceeded=false;
    }
#endif    

#if defined(CONFIG_LPS_HUM_CHANGE_PUBLISH_THRESHOLD) \
    && CONFIG_LPS_HUM_CHANGE_PUBLISH_THRESHOLD != 0
    LOG_DBG("humidity change is %d",
	   abs(mbox_message.rh_x10 - most_recent_published_hum));
    if (abs(mbox_message.rh_x10 - most_recent_published_hum) <
	CONFIG_LPS_HUM_CHANGE_PUBLISH_THRESHOLD) {
	humidity_threshold_exceeded=false;
    }
#endif    
    
    if (time_threshold_exceeded ||
	temperature_threshold_exceeded ||
	humidity_threshold_exceeded) {

        if (!waiting_for_ack) {
            LOG_DBG("Threshold met. Waking HP core.");
            
            // Give the HP core the return address for the receipt
            mbox_message.most_recent_publish_status_p = &hp_publish_status;
            hp_publish_status = PUBLISH_STATUS_PENDING; // Reset to pending
            waiting_for_ack = true;

#ifndef CONFIG_LPS_HPCORE_ALWAYS_STAY_AWAKE	    
	    ulp_lp_core_wakeup_main_processor();
#endif	    
	    mbox_message.hp_wake_count++;

	
	    msg.data = &mbox_message;
	    msg.size = mbox_message_size;
	
            // Collapsed into a single string macro
	    LOG_DBG("Calling mbox_send with:\n" \
                    "  .lp_wake_count=%d\n" \
		    "  .hp_wake_count=%d\n" \
		    "  .temp_c_x10=%d .rh_x10=%d\n" \
		    "  .most_recent_publish_status_p=0x%x",		\
		   ((lp_to_hp_shared_data_t *) msg.data)->lp_wake_count, \
		   ((lp_to_hp_shared_data_t *) msg.data)->hp_wake_count, \
		   ((lp_to_hp_shared_data_t *) msg.data)->temp_c_x10, \
		   ((lp_to_hp_shared_data_t *) msg.data)->rh_x10, \
		   (uint32_t)((lp_to_hp_shared_data_t *) msg.data)->most_recent_publish_status_p);

	    LOG_DBG("  *(most_recent_publish_status_p) = %d", \
		    (uint32_t)*(((lp_to_hp_shared_data_t *) msg.data)->most_recent_publish_status_p));
		
	    mbox_message.shared_magic = SHARED_DATA_MAGIC_NUMBER;
	    if (mbox_send_dt(&tx_channel, &msg) < 0) {
		LOG_ERR("mbox_send() error");
	    }
	
	    k_msleep(100);
	} else {
            LOG_DBG("Skipping HP wake. Actively waiting for network receipt.");
            wakeups_since_last_publish++;
        }	
    }
    else {
	wakeups_since_last_publish++;
    }

    

    LOG_DBG("LP core Going to sleep");
        
    if (cfg->sleep_duration_ticks) {
	ulp_lp_core_lp_timer_set_wakeup_ticks
	    (cfg->sleep_duration_ticks);
    }
	
    //Busy wait to let the printf UART finish
    ulp_lp_core_delay_us(500);
    
    ulp_lp_core_halt();

    return 0;
}
