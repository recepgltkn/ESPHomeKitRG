#include <homekit/homekit.h>
#include <homekit/characteristics.h>

void accessory_identify(homekit_value_t _value) {
  printf("identify\n");
}

homekit_characteristic_t cha_switch_on = HOMEKIT_CHARACTERISTIC_(ON, false);
homekit_characteristic_t cha_switch_brightness = HOMEKIT_CHARACTERISTIC_(BRIGHTNESS, 100);
homekit_characteristic_t cha_name = HOMEKIT_CHARACTERISTIC_(NAME, "Wemos Role");
homekit_characteristic_t cha_accessory_name = HOMEKIT_CHARACTERISTIC_(NAME, "Wemos Role");
homekit_characteristic_t cha_serial_number = HOMEKIT_CHARACTERISTIC_(SERIAL_NUMBER, "WEMOS-D1-R2-RELAY");
homekit_characteristic_t cha_firmware_revision = HOMEKIT_CHARACTERISTIC_(FIRMWARE_REVISION, "1.0.0");
homekit_characteristic_t cha_temperature = HOMEKIT_CHARACTERISTIC_(CURRENT_TEMPERATURE, 20.0);
homekit_characteristic_t cha_humidity = HOMEKIT_CHARACTERISTIC_(CURRENT_RELATIVE_HUMIDITY, 50.0);
homekit_characteristic_t cha_motion_detected = HOMEKIT_CHARACTERISTIC_(MOTION_DETECTED, false);
homekit_characteristic_t cha_light_level = HOMEKIT_CHARACTERISTIC_(CURRENT_AMBIENT_LIGHT_LEVEL, 10.0);
homekit_characteristic_t cha_aux1_on = HOMEKIT_CHARACTERISTIC_(ON, false);
homekit_characteristic_t cha_aux1_brightness = HOMEKIT_CHARACTERISTIC_(BRIGHTNESS, 100);
homekit_characteristic_t cha_aux2_on = HOMEKIT_CHARACTERISTIC_(ON, false);
homekit_characteristic_t cha_aux2_brightness = HOMEKIT_CHARACTERISTIC_(BRIGHTNESS, 100);

homekit_accessory_t *accessories[] = {
    HOMEKIT_ACCESSORY(.id = 1, .category = homekit_accessory_category_lightbulb,
        .services = (homekit_service_t*[]) {
            HOMEKIT_SERVICE(ACCESSORY_INFORMATION,
                .characteristics = (homekit_characteristic_t*[]) {
                    &cha_accessory_name,
                    HOMEKIT_CHARACTERISTIC(MANUFACTURER, "Recep"),
                    &cha_serial_number,
                    HOMEKIT_CHARACTERISTIC(MODEL, "D1MiniRelay"),
                    &cha_firmware_revision,
                    HOMEKIT_CHARACTERISTIC(IDENTIFY, accessory_identify),
                    NULL
                }),
            HOMEKIT_SERVICE(LIGHTBULB, .primary = true,
                .characteristics = (homekit_characteristic_t*[]) {
                    &cha_switch_on,
                    &cha_switch_brightness,
                    &cha_name,
                    HOMEKIT_CHARACTERISTIC(NAME, "Ana Isik"),
                    NULL
                }),
            HOMEKIT_SERVICE(LIGHTBULB,
                .characteristics = (homekit_characteristic_t*[]) {
                    HOMEKIT_CHARACTERISTIC(NAME, "PWM 1"),
                    &cha_aux1_on,
                    &cha_aux1_brightness,
                    NULL
                }),
            HOMEKIT_SERVICE(LIGHTBULB,
                .characteristics = (homekit_characteristic_t*[]) {
                    HOMEKIT_CHARACTERISTIC(NAME, "PWM 2"),
                    &cha_aux2_on,
                    &cha_aux2_brightness,
                    NULL
                }),
            HOMEKIT_SERVICE(TEMPERATURE_SENSOR,
                .characteristics = (homekit_characteristic_t*[]) {
                    HOMEKIT_CHARACTERISTIC(NAME, "Sicaklik"),
                    &cha_temperature,
                    NULL
                }),
            HOMEKIT_SERVICE(HUMIDITY_SENSOR,
                .characteristics = (homekit_characteristic_t*[]) {
                    HOMEKIT_CHARACTERISTIC(NAME, "Nem"),
                    &cha_humidity,
                    NULL
                }),
            HOMEKIT_SERVICE(MOTION_SENSOR,
                .characteristics = (homekit_characteristic_t*[]) {
                    HOMEKIT_CHARACTERISTIC(NAME, "Hareket"),
                    &cha_motion_detected,
                    NULL
                }),
            HOMEKIT_SERVICE(LIGHT_SENSOR,
                .characteristics = (homekit_characteristic_t*[]) {
                    HOMEKIT_CHARACTERISTIC(NAME, "Isik"),
                    &cha_light_level,
                    NULL
                }),
            NULL
        }),
    NULL
};

homekit_server_config_t config = {
    .accessories = accessories,
    .password = "111-11-111"
};
