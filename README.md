# low_power_sensor_esp32c

This project uses Zephyr, an esp32c6 SoC and an AHT20 sensor to implement an integrated sensor that samples humidity and temperature using the AHT sensor and produces MQTT events representing those samples periodically and whenever a value has exceeded a configurable threshold. By default, the WiFi is used as the transport, but it can also be configured to use Thread instead.

Low power is achieved using the esp32c6's dual core design, which includes a "low power core" (LP core) and a "high performance core" (HP core). The idea is that the LP core is exclusively used to sample temperature and humidity over a dedicated i2c bus, while the HP core is only woken from deep sleep to send MQTT events. Both cores are placed into a "deep sleep" when they're not in use. During deep sleep, the only device that remains powered is a small region of static ram (RTC SRAM) that maintains selected variables during the sleep state. 

## Configuring WiFi credentials
To configure the SSID and PSK for your WiFi network, edit these lines in `snippets/wifi-mqtt/wifi-mqtt.conf`:
```
CONFIG_WIFI_CREDENTIALS_STATIC_SSID="<your ssid>"
CONFIG_WIFI_CREDENTIALS_STATIC_PASSWORD="<your psk>"
```

## Configuring MQTT broker address and credentials:
You'll also need to configure the address, port, username, and password of the MQTT broker that the device will publish to by changing these lines in `snippets/wifi-mqtt/wifi-mqtt.conf`
```
CONFIG_LPS_MQTT_BROKER_ADDR="192.168.1.3"
CONFIG_LPS_MQTT_BROKER_PORT=1883
CONFIG_LPS_MQTT_BROKER_USERNAME="sensor_user"
CONFIG_LPS_MQTT_BROKER_PASSWORD="$en$or_pa$$word"
```

## Building and Flashing the MQTT over WiFi image

Zephyr's "west" build tool coordinates building all 3 images (mcuboot, low-power core image, and high-performance core image) with one command:
```
west build --sysbuild -p always -b esp32c6_devkitc/esp32c6/hpcore . -- -Dlow_power_sensor_esp32c_SNIPPET="espressif-flash-4M;wifi-mqtt"
```

Flashing all 3 images can also be accomplished with a single command:
```
west flash --esp-device /dev/ttyACM0
```



## Configuring and Building the MQTT-SN over Thread image

The esp32c6 contains an 802.15.4 radio, so the image can be configured to attach to a Thread mesh rather than a WiFi network. I'm not going to document the entire process of setting up a Thread network here, but you'll need the following additional hardware components:
- A general purpose computer with a thread interface. The easiest way to accomplish this is usually to purchase a USB Zigbee dongle and re-flash it with Thread firmware.
You'll also need the following software components:
- An OpenThread border router. In Linux, this will usuall be `ot-br-posix`, which can either be built from source or run from a prebuilt Docker container.
- An MQTT-SN gateway. This also needs to be built from source or run from a prebuilt Docker container.
If you're already running the WiFi version of `low_power_sensor_esp32c', you can set up the Thread dongle, the OpenThread border router, and the MQTT-SN gateway on the same general purpose computer that you're already running the MQTT broker on. This allows you to have both Thread devices and WiFI devices publish events to the same MQTT broker.

To configure the Thread mesh credentials and MQTT-SN gateway port/address, edit these lines in `snippets/thread-mqttsn/thread-mqttsn.conf`:
```
# The IPv6 address of your MQTT-SN gateway (e.g., the Raspberry Pi)
CONFIG_LPS_MQTT_BROKER_ADDR="ffaa:aaff:abcd:ef01:23ab:cdef:1111:3333"
CONFIG_LPS_MQTT_BROKER_PORT=1884

# --- OpenThread Network Credentials ---
CONFIG_OPENTHREAD_NETWORK_NAME="OpenThread-abc0"
CONFIG_OPENTHREAD_NETWORKKEY="01:23:45:67:89:ab:cd:ef:00:11:22:aa:bb:cc:dd:ee"
CONFIG_OPENTHREAD_PANID=12345
CONFIG_OPENTHREAD_XPANID="a0:b1:c2:d3:e4:f5:a1:b2"
CONFIG_OPENTHREAD_CHANNEL=22
```

To build the image that uses Thread and MQTT-SN, use this `west` command:
```
west build --sysbuild -p always -b esp32c6_devkitc/esp32c6/hpcore . -- -Dlow_power_sensor_esp32c_SNIPPET="espressif-flash-4M;thread-mqttsn"
```

Flashing the 3 images is done with the usual:
```
west flash --esp-device /dev/ttyACM0
```
