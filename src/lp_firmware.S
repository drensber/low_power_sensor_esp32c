/* Define the section - .rodata puts it in Flash (not RAM) */
.section .rodata

/* Export the symbols so main.c can see them */
.global ulp_lp_core_app_start
.global ulp_lp_core_app_end

/* The Labels */
ulp_lp_core_app_start:
    /* INCBIN includes the raw binary bytes */
    /* Update this path to match your build output structure */
    .incbin "../low_power_sensor_lpcore/zephyr/zephyr_stripped.bin" 

ulp_lp_core_app_end:
