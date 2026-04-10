# low_power_sensor_esp32c

This project uses an esp32c6 SoC and an AHT20 sensor to implement an integrated sensor that samples humidity and temperature using the AHT sensor and produces MQTT events representing those samples periodically and whenever a value has exceeded a configurable threshold.

Low power is achieved using the esp32c6's dual core design, which includes a "low power core" (LP core) and a "high performance core" (HP core). The idea is that the LP core is exclusively used to sample temperature and humidity over a dedicated i2c bus, while the HP core is only woken from deep sleep to send MQTT events. Both cores are placed into a "deep sleep" when they're not in use. During "deep sleep", the only device that remains powered is a small region of static ram (RTC SRAM) that maintains selected variables during the sleep state. 
