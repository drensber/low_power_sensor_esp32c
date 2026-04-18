#define AHT20_I2C_ADDR 0x38
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include "shared_data.h"

LOG_MODULE_DECLARE(lps_lp, CONFIG_LPS_LOG_LEVEL);

#if 1
// The lp_i2c API isn't supported yet for the esp32c6.
// This is a workaround implementation that uses the esp-idf to do software bitbanging instead.

#include "ulp_lp_core_gpio.h" // Drop to the lowest level GPIO macros

#define SCL_PIN 6
#define SDA_PIN 7
#define AHT20_ADDR AHT20_I2C_ADDR

// --- 1. Raw GPIO Open-Drain Primitives ---
static void i2c_delay(void) {
    // A crude but effective microsecond delay.
    // LP core runs at roughly 16-20MHz. This safely gives us ~5us for 100kHz I2C.
    for (volatile int i = 0; i < 25; i++); 
}

static void sda_high(void) { ulp_lp_core_gpio_output_disable(SDA_PIN); }
static void sda_low(void)  { ulp_lp_core_gpio_output_enable(SDA_PIN); ulp_lp_core_gpio_set_level(SDA_PIN, 0); }
static void scl_high(void) { ulp_lp_core_gpio_output_disable(SCL_PIN); }
static void scl_low(void)  { ulp_lp_core_gpio_output_enable(SCL_PIN); ulp_lp_core_gpio_set_level(SCL_PIN, 0); }

// --- 2. I2C Protocol Logic ---
static void i2c_start(void) {
    sda_high(); scl_high(); i2c_delay();
    sda_low();  i2c_delay();
    scl_low();  i2c_delay();
}

static void i2c_stop(void) {
    sda_low();  scl_low();  i2c_delay();
    scl_high(); i2c_delay();
    sda_high(); i2c_delay();
}

static void i2c_write_bit(int bit) {
    if (bit) sda_high(); else sda_low();
    i2c_delay();
    scl_high(); i2c_delay();
    scl_low();  i2c_delay();
}

static int i2c_read_bit(void) {
    sda_high(); i2c_delay(); // Release SDA to read
    scl_high(); i2c_delay();
    int bit = ulp_lp_core_gpio_get_level(SDA_PIN);
    scl_low();  i2c_delay();
    return bit;
}

static int i2c_write_byte(uint8_t data) {
    for (int i = 0; i < 8; i++) {
        i2c_write_bit((data & 0x80) != 0);
        data <<= 1;
    }
    return i2c_read_bit(); // Returns the ACK bit (0 = ACK, 1 = NACK)
}

static uint8_t i2c_read_byte(int ack) {
    uint8_t data = 0;
    for (int i = 0; i < 8; i++) {
        data = (data << 1) | i2c_read_bit();
    }
    i2c_write_bit(ack); // 0 = ACK, 1 = NACK
    return data;
}

// --- 3. The AHT20 Sensor Routine ---
void read_aht20(lp_to_hp_shared_data_t *return_data) {
    // Init pins as inputs (pulled high by external resistors)
    ulp_lp_core_gpio_init(SCL_PIN);
    ulp_lp_core_gpio_init(SDA_PIN);

    // 2. THE MISSING LINK: Enable the input buffers so we can actually read the line!
    ulp_lp_core_gpio_input_enable(SCL_PIN);
    ulp_lp_core_gpio_input_enable(SDA_PIN);
    
    // 3. Enable internal pull-ups (belt and suspenders)
    ulp_lp_core_gpio_pullup_enable(SCL_PIN);
    ulp_lp_core_gpio_pullup_enable(SDA_PIN);    

    
    // Trigger Measurement
    i2c_start();
    if (i2c_write_byte(AHT20_ADDR << 1) != 0) {
	LOG_DBG("i2c write error 1");
	return; // Write Addr (NACKed)
    }
    i2c_write_byte(0xAC);
    i2c_write_byte(0x33);
    i2c_write_byte(0x00);
    i2c_stop();

    k_msleep(80); // We can still use Zephyr's low-res sleep here

    // Read Data
    i2c_start();
    if (i2c_write_byte((AHT20_ADDR << 1) | 1) != 0) {
	LOG_DBG("i2c write error 2");
	return; // Read Addr
    }
    
    uint8_t data[6];
    for (int i = 0; i < 6; i++) {
        // Send ACK (0) for first 5 bytes, NACK (1) for the last byte
        data[i] = i2c_read_byte((i == 5) ? 1 : 0); 
    }
    i2c_stop();

    if ((data[0] & 0x80) != 0) {
	LOG_DBG("i2c sensor busy");
	return; // Sensor busy
    }
    
    uint32_t raw_temp = ((data[3] & 0x0F) << 16) | (data[4] << 8) | data[5];

    //return (int32_t)((raw_temp * 625) >> 15) - 5000;
    return_data->temp_c_x10 = ((raw_temp * 250) >> 17) - 500;

    uint32_t raw_humidity = ((data[1] << 12) | (data[2] << 4) | (data[3] >> 4));
    return_data->rh_x10 = (raw_humidity * 125) >> 17;
    
}
#else // 1
// This is the the Zephyr lp_i2c hardware i2c API implementation. Switch once supported
#include <zephyr/drivers/i2c.h>

// Get the I2C bus pointer from the Devicetree
const struct device *i2c_dev = DEVICE_DT_GET(DT_NODELABEL(lp_i2c0));

int32_t read_aht20_temp(void) {
    if (!device_is_ready(i2c_dev)) {
        return 0; // Bus not ready
    }

    // 1. Trigger Measurement Command
    uint8_t cmd[3] = {0xAC, 0x33, 0x00};
    if (i2c_write(i2c_dev, cmd, 3, AHT20_I2C_ADDR) != 0) {
        return 0; // Write failed
    }

    // 2. Wait for the sensor to process (Datasheet says 80ms)
    k_msleep(80);

    // 3. Read the 6 bytes of data
    uint8_t data[6] = {0};
    if (i2c_read(i2c_dev, data, 6, AHT20_I2C_ADDR) != 0) {
        return 0; // Read failed
    }

    // Check if the sensor is still busy (Bit 7 of Byte 0)
    if ((data[0] & 0x80) != 0) {
        return 0; // Sensor didn't finish in time
    }

    // 4. Extract Temperature (Bottom half of Byte 3, plus Bytes 4 and 5)
    uint32_t raw_temp = ((data[3] & 0x0F) << 16) | (data[4] << 8) | data[5];

    // 5. Integer Math! (Instead of T = (raw/2^20)*200 - 50, we multiply by 20000 first)
    // This gives us the temperature in Celsius * 100 (e.g., 2250 = 22.50 C)
    int32_t temp_c_x100 = (int32_t)((raw_temp * 20000) >> 20) - 5000;

    return temp_c_x100;
}
#endif
