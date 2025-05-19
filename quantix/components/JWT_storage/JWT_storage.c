#include "nvs.h"
#include "nvs_flash.h"
#include <stdlib.h>
#include <string.h>

char *jwt_load_from_nvs() {
    nvs_handle_t handle;
    size_t required_size = 0;
    if (nvs_open("storage", NVS_READONLY, &handle) != ESP_OK)
        return NULL;

    if (nvs_get_str(handle, "jwt", NULL, &required_size) != ESP_OK) {
        nvs_close(handle);
        return NULL;
    }

    char *jwt = malloc(required_size);
    nvs_get_str(handle, "jwt", jwt, &required_size);
    nvs_close(handle);
    return jwt;
}

void jwt_save_to_nvs(const char *jwt) {
    nvs_handle_t handle;
    if (nvs_open("storage", NVS_READWRITE, &handle) != ESP_OK)
        return;

    nvs_set_str(handle, "jwt", jwt);
    nvs_commit(handle);
    nvs_close(handle);
}
