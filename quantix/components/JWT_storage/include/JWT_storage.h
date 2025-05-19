#ifndef JWT_STORAGE_H
#define JWT_STORAGE_H

char* jwt_load_from_nvs();
void jwt_save_to_nvs(const char* jwt);

#endif
