/*
 * Copyright (c) 2025 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/sys/poweroff.h>
#include <esp_attr.h>

#include <esp_sleep.h>
#include <ulp_lp_core.h>

#ifdef CONFIG_LPS_EXPLICIT_LP_IMAGE_LOADING    
extern const uint8_t ulp_lp_core_app_start[];
extern const uint8_t ulp_lp_core_app_end[];
#endif // CONFIG_LPS_EXPLICIT_LP_IMAGE_LOADING

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

    
    printk(">>> HP SLEEP: Entering Deep Sleep... <<<\n");
    
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
