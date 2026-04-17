/*
 * main() routine for the high performance core image.
 *
 * Copyright (c) 2025 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/mbox.h>
#include <zephyr/sys/poweroff.h>
#include <esp_attr.h>

#include <esp_sleep.h>
#include <ulp_lp_core.h>

#include "shared_data.h"

extern bool lps_send_update(lp_to_hp_shared_data_t *);

#ifdef CONFIG_LPS_EXPLICIT_LP_IMAGE_LOADING    
extern const uint8_t ulp_lp_core_app_start[];
extern const uint8_t ulp_lp_core_app_end[];
#endif // CONFIG_LPS_EXPLICIT_LP_IMAGE_LOADING

static RTC_DATA_ATTR uint32_t hp_wake_count = 0;
static RTC_DATA_ATTR uint32_t callback_count = 0;
static RTC_DATA_ATTR bool first_boot=true;

const struct mbox_dt_spec rx_channel =
    MBOX_DT_SPEC_GET(DT_PATH(mbox_consumer), rx);
static lp_to_hp_shared_data_t g_mbox_received_data;
static mbox_channel_id_t g_mbox_received_channel;

static volatile bool mbox_message_received = false;


static void callback(const struct device *dev,
		     mbox_channel_id_t channel_id,
		     void *user_data,
		     struct mbox_msg *data)
{
    printk("calling callback(channel_id=%d, user_data=%x, data=%x\n",
	   (uint32_t) channel_id, (uint32_t)user_data, (uint32_t)data);
    printk("                 data->size=%d, data->data=%x\n",
	   (uint32_t)data->size, (uint32_t)data->data);
    if (callback_count++ > 0) {
	memcpy(&g_mbox_received_data, data->data,
	       sizeof(lp_to_hp_shared_data_t));

	g_mbox_received_channel = channel_id;
    
	mbox_message_received = true;
    }
}


int main(void)
{
    uint32_t cause;
    bool successful_publish;
    
#if defined(CONFIG_LPS_USE_LIGHT_SLEEP) \
    || defined(CONFIG_LPS_HPCORE_ALWAYS_STAY_AWAKE)
    while (1) {
#endif
    // 1. Check wakeup cause
    hwinfo_get_reset_cause(&cause);

#if !(defined(CONFIG_LPS_USE_LIGHT_SLEEP) \
      || defined(CONFIG_LPS_HPCORE_ALWAYS_STAY_AWAKE))    
    if (cause & RESET_LOW_POWER_WAKE || hp_wake_count == 0) {
#else
    if (true) {
#endif
        printk(">>> HP WAKE: Woken by LP Core! <<<\n");

	if (mbox_register_callback_dt(&rx_channel, callback, NULL)) {
	    printk("mbox_register_callback() error\n");
	    return 0;
	}
	else {
	    printk("registered mbox callback\n");
	}

	if (mbox_set_enabled_dt(&rx_channel, 1)) {
	    printk("mbox_set_enable() error\n");
	    return 0;
	}
	else {
	    printk("enabled mbox\n");
	}

	
	printk("Waiting for message from LP core to appear in mbox\n");
	
	while (!mbox_message_received) {
	    k_msleep(50);
	}

	printk("mbox message received\n");

	printk("Message value received:\n"
	       "  .lp_wake_count=%d\n"
	       "  .hp_wake_count=%d\n"
	       "  .temp_c_x10=%d\n"
	       "  .rh_x10=%d\n",
	       g_mbox_received_data.lp_wake_count,
	       g_mbox_received_data.hp_wake_count,
	       g_mbox_received_data.temp_c_x10,
	       g_mbox_received_data.rh_x10);
	
	mbox_message_received = false;

	successful_publish = lps_send_update(&g_mbox_received_data);

	printk("successful_publish = %s\n", successful_publish ? "true" : "false");

	// Write the receipt directly to the LP core's RTC memory!
	if (g_mbox_received_data.most_recent_publish_status_p != NULL) {
	    *(g_mbox_received_data.most_recent_publish_status_p) =
		successful_publish ? PUBLISH_STATUS_SUCCESS : PUBLISH_STATUS_FAILURE;
	    printk("Wrote receipt %d to LP core.\n",
		*(g_mbox_received_data.most_recent_publish_status_p));
	}
	
        k_msleep(50); // Allow logs to flush
    }

    else if (first_boot)
    {	
	printk(">>> HP BOOT: Cold start.\n");
#ifdef CONFIG_LPS_EXPLICIT_LP_IMAGE_LOADING    	
        printk("   Loading LP firmware... ");

        /* Load LP firmware into RTC RAM */
        ulp_lp_core_load_binary(ulp_lp_core_app_start,
				(ulp_lp_core_app_end - ulp_lp_core_app_start));

	ulp_lp_core_cfg_t cfg = {
            .wakeup_source = ULP_LP_CORE_WAKEUP_SOURCE_HP_CPU,
        };
	
        /* Start the LP Core */
        ulp_lp_core_run(&cfg);

	first_boot=false;
	
	printk(" Done loading firmware. LP core started.\n");
#endif // CONFIG_LPS_EXPLICIT_LP_IMAGE_LOADING
    }

    
    printk(">>> HP SLEEP: Entering Deep Sleep... %d <<<\n", hp_wake_count);
    
    hp_wake_count++;
	
    // 2. Allow UART to flush before cutting power
    k_msleep(50); 

    // 3. Enable ULP wakeup (keeps LP core powered on!)
    esp_sleep_enable_ulp_wakeup();

    // 4. Power down or put the HP core to sleep
#ifndef CONFIG_LPS_HPCORE_ALWAYS_STAY_AWAKE
#ifndef CONFIG_LPS_USE_LIGHT_SLEEP
    esp_deep_sleep_start();
#else
    esp_light_sleep_start();
#endif // CONFIG_LPS_USE_LIGHT_SLEEP
#endif // CONFIG_LPS_HPCORE_ALWAYS_STAY_AWAKE

#if defined(CONFIG_LPS_USE_LIGHT_SLEEP) \
    || defined(CONFIG_LPS_HPCORE_ALWAYS_STAY_AWAKE)    
    } // while (1) infinite loop
#endif    

}
