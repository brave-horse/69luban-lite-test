#ifndef WIFI_CREDENTIALS_H
#define WIFI_CREDENTIALS_H

#include <stdbool.h>

/* 仅由 WiFi 工作线程调用，凭据持久化统一通过 app_storage。 */
void wifi_credentials_load(void);
const char *wifi_credentials_find(const char *ssid);
bool wifi_credentials_save(const char *ssid, const char *password);
bool wifi_credentials_remove(const char *ssid);

#endif
