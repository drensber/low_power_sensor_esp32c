/*
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

#ifdef CONFIG_LPS_EXPLICIT_LP_IMAGE_LOADING    
extern const uint8_t ulp_lp_core_app_start[];
extern const uint8_t ulp_lp_core_app_end[];
#endif // CONFIG_LPS_EXPLICIT_LP_IMAGE_LOADING

static RTC_DATA_ATTR uint32_t hp_wake_count;


const RTC_DATA_ATTR struct mbox_dt_spec rx_channel = MBOX_DT_SPEC_GET(DT_PATH(mbox_consumer), rx);
static mbox_channel_id_t g_mbox_received_data;
static mbox_channel_id_t g_mbox_received_channel;

static bool mbox_message_received = false;

#if 1
static void callback(const struct device *dev, mbox_channel_id_t channel_id, void *user_data,
		     struct mbox_msg *data)
{
    printk("calling callback(channel_id=%d, user_data=%x, data=%x\n", (uint32_t) channel_id, (uint32_t)user_data, (uint32_t)data);
    printk("                 data->size=%d, data->data=%x\n", (uint32_t)data->size, (uint32_t)data->data);
    memcpy(&g_mbox_received_data, data->data, 4);

    g_mbox_received_channel = channel_id;
    mbox_message_received = true;
}
#endif

int main(void)
{
    uint32_t cause;

    
#ifdef CONFIG_LPS_USE_LIGHT_SLEEP 
    static bool first_boot=true;
    
    while (1) {
#endif //CONFIG_LPS_USE_LIGHT_SLEEP
	
    // 1. Check wakeup cause
    hwinfo_get_reset_cause(&cause);

    if (cause & RESET_LOW_POWER_WAKE) {
        printk(">>> HP WAKE: Woken by LP Core! <<<\n");
        // TODO: Do your WiFi/Bluetooth work here

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

	printk("Message value %d\n", (uint32_t) g_mbox_received_data);
	
	mbox_message_received = false;

        k_msleep(50); // Allow logs to flush
    }

#ifdef CONFIG_LPS_USE_LIGHT_SLEEP    
    else if (first_boot)
#else
    else
#endif //CONFIG_LPS_USE_LIGHT_SLEEP
    {	
	printk(">>> HP BOOT: Cold start.\n");
#ifdef CONFIG_LPS_EXPLICIT_LP_IMAGE_LOADING    	
        printk("   Loading LP firmware... ");

        /* Load LP firmware into RTC RAM */
        ulp_lp_core_load_binary(ulp_lp_core_app_start, (ulp_lp_core_app_end - ulp_lp_core_app_start));

	ulp_lp_core_cfg_t cfg = {
            .wakeup_source = ULP_LP_CORE_WAKEUP_SOURCE_HP_CPU,
        };
	
        /* Start the LP Core */
        ulp_lp_core_run(&cfg);

#ifdef CONFIG_LPS_USE_LIGHT_SLEEP   	
	first_boot=false;
#endif
	
	printk(" Done loading firmware. LP core started.\n");
#endif // CONFIG_LPS_EXPLICIT_LP_IMAGE_LOADING
    }

    
    printk(">>> HP SLEEP: Entering Deep Sleep... %d <<<\n", hp_wake_count++);
    
    // 2. Allow UART to flush before cutting power
    k_msleep(50); 

    // 3. Enable ULP wakeup (keeps LP core powered on!)
    esp_sleep_enable_ulp_wakeup();

    // 4. Power down or put the HP core to sleep
#ifndef CONFIG_LPS_USE_LIGHT_SLEEP   	
    esp_deep_sleep_start();
#else
    esp_light_sleep_start();

    } // while (1) infinite loop
#endif    
}
