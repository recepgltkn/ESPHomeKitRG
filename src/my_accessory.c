#include <homekit/homekit.h>
#include <homekit/characteristics.h>

void accessory_identify(homekit_value_t _value) {
  printf("identify\n");
}

homekit_characteristic_t cha_switch_on = HOMEKIT_CHARACTERISTIC_(ON, false);
homekit_characteristic_t cha_name = HOMEKIT_CHARACTERISTIC_(NAME, "Wemos Role");

homekit_accessory_t *accessories[] = {
    HOMEKIT_ACCESSORY(.id = 1, .category = homekit_accessory_category_switch,
        .services = (homekit_service_t*[]) {
            HOMEKIT_SERVICE(ACCESSORY_INFORMATION,
                .characteristics = (homekit_characteristic_t*[]) {
                    HOMEKIT_CHARACTERISTIC(NAME, "Wemos Role"),
                    HOMEKIT_CHARACTERISTIC(MANUFACTURER, "Recep"),
                    HOMEKIT_CHARACTERISTIC(SERIAL_NUMBER, "WEMOS-D1-R2-RELAY"),
                    HOMEKIT_CHARACTERISTIC(MODEL, "D1MiniRelay"),
                    HOMEKIT_CHARACTERISTIC(FIRMWARE_REVISION, "1.0.0"),
                    HOMEKIT_CHARACTERISTIC(IDENTIFY, accessory_identify),
                    NULL
                }),
            HOMEKIT_SERVICE(SWITCH, .primary = true,
                .characteristics = (homekit_characteristic_t*[]) {
                    &cha_switch_on,
                    &cha_name,
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
