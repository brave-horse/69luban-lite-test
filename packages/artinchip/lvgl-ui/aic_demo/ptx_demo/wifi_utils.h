#ifndef __WIFI_UTILS_H__
#define __WIFI_UTILS_H__

#include <stdbool.h>
#include <stdint.h>

#include "wifi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_DEVICE              "wlan0"
#define WIFI_POLL_MS             200
#define WIFI_SCAN_TIMEOUT_MS     10000
#define WIFI_JOIN_TIMEOUT_MS     30000
#define WIFI_RETRY_MS            15000
#define WIFI_THREAD_STACK_SIZE   (24U * 1024U)

typedef enum {
    WIFI_CMD_NONE,
    WIFI_CMD_CONNECT,
    WIFI_CMD_DISCONNECT,
    WIFI_CMD_REMOVE,
} wifi_command_t;

typedef struct {
    wifi_command_t command;
    char ssid[33];
    char password[WIFI_PASSWORD_MAX_LEN + 1];
    bool saved;
    bool temporary;
} wifi_request_t;

#define WIFI_UTILS_EN
#ifdef WIFI_UTILS_EN

int wifi_init(void);
bool wifi_is_enabled(void);
int wifi_request_set_enabled(bool enabled);
int wifi_get_ip(char *ip, int len);
int wifi_get_mac(char *mac, int len);
int wifi_get_rssi(int *rssi);
int wifi_request_connect(const char *ssid, const char *password, bool temporary);
int wifi_disconnect(void);
int wifi_request_scan(void);
int wifi_get_scan_result(wifi_scan_result_t *result);
int wifi_request_remove_network(const char *ssid);
wifi_state_t wifi_get_current_state(void);

#else

#define wifi_init()                                      (0)
#define wifi_is_enabled()                                (false)
#define wifi_request_set_enabled(enabled)                (0)
#define wifi_get_ip(ip, len)                             (-1)
#define wifi_get_mac(mac, len)                           (-1)
#define wifi_get_rssi(rssi)                              (-1)
#define wifi_request_connect(ssid, password, temporary) (-1)
#define wifi_disconnect()                                (-1)
#define wifi_request_scan()                              (-1)
#define wifi_get_scan_result(result)                     (-1)
#define wifi_request_remove_network(ssid)                (-1)
#define wifi_get_current_state()                         WIFI_STATE_DISCONNECTED

#endif

#ifdef __cplusplus
}
#endif

#endif
