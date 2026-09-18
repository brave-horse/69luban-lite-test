#ifndef __WIFI_UTILS_H__
#define __WIFI_UTILS_H__

#include <stdbool.h>
#include <stdint.h>

#include "wifi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* WiFi 工作线程会进入文件系统和 NAND 写入调用链，需要预留较大的栈空间。 */
#define WIFI_DEVICE              "wlan0"       // WiFi 设备名
#define WIFI_POLL_MS             200             // 工作线程轮询周期
#define WIFI_SCAN_TIMEOUT_MS     10000           // 扫描超时时间
#define WIFI_JOIN_TIMEOUT_MS     30000           // 连接超时时间
#define WIFI_RETRY_MS            15000           // 自动重连间隔
#define WIFI_THREAD_STACK_SIZE   (24U * 1024U)   // WiFi 工作线程栈，预留凭据处理及驱动调用开销

// WiFi 请求类型和参数。
typedef enum
{
    WIFI_CMD_NONE,                             // 无待处理命令
    WIFI_CMD_CONNECT,                          // 连接网络
    WIFI_CMD_DISCONNECT,                       // 断开网络
    WIFI_CMD_REMOVE,                           // 删除网络
} wifi_command_t;


// WiFi 请求结构体，提交给后台线程处理。
typedef struct
{
    wifi_command_t command;                    // 待处理命令
    char ssid[33];                             // 目标网络名称
    char password[WIFI_PASSWORD_MAX_LEN + 1];   // 网络密码，包含字符串结束符
    bool saved;                                // 是否使用已保存密码
} wifi_request_t;

#define WIFI_UTILS_EN
#ifdef WIFI_UTILS_EN

/* 初始化 WiFi 管理模块。 */
int wifi_init(void);

/* 查询 WLAN 是否已启用。 */
bool wifi_is_enabled(void);

/* 提交 WLAN 开关请求，由 WiFi 后台线程处理。 */
int wifi_request_set_enabled(bool enabled);

/* 获取当前 WLAN 的 IP 地址。 */
int wifi_get_ip(char *ip, int len);

/* 获取 WLAN MAC 地址。 */
int wifi_get_mac(char *mac, int len);

/* 获取当前 WiFi 信号强度。 */
int wifi_get_rssi(int *rssi);

/* 提交连接请求：NULL 使用已保存密码，空字符串连接开放网络。 */
int wifi_request_connect(const char *ssid, const char *password);

/* 异步断开当前 WiFi。 */
int wifi_disconnect(void);

/* 请求 WiFi 后台线程执行一次扫描。 */
int wifi_request_scan(void);

/* 复制当前已发布的 WiFi 扫描快照。 */
int wifi_get_scan_result(wifi_scan_result_t *result);

/* 提交删除指定 WiFi 配置的请求。 */
int wifi_request_remove_network(const char *ssid);

/* 获取当前 WiFi 连接状态。 */
wifi_state_t wifi_get_current_state(void);
#else

#define wifi_init()                             (0)
#define wifi_is_enabled()                       (false)
#define wifi_request_set_enabled(enabled)       (0)
#define wifi_get_ssid(ssid, len)                (-1)
#define wifi_get_ip(ip, len)                    (-1)
#define wifi_get_mac(mac, len)                  (-1)
#define wifi_get_rssi(rssi)                     (-1)
#define wifi_request_connect(ssid, password)    (-1)
#define wifi_disconnect()                       (-1)
#define wifi_request_scan()                     (-1)
#define wifi_get_scan_result(result)            (-1)
#define wifi_request_remove_network(ssid)       (-1)
#define wifi_get_current_state()                WIFI_STATE_DISCONNECTED


#endif

#ifdef __cplusplus
}
#endif

#endif
