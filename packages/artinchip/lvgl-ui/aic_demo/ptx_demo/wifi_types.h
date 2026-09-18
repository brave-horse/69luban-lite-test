#ifndef __WIFI_TYPES_H__
#define __WIFI_TYPES_H__

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* WiFi 连接状态。 */
typedef enum {
    WIFI_STATE_DISCONNECTED = 0,    // 未连接
    WIFI_STATE_CONNECTING,          // 正在连接
    WIFI_STATE_CONNECTED,           // 已连接
    WIFI_STATE_AUTH_FAILED,         // 认证失败
    WIFI_STATE_CONNECT_TIMEOUT,     // 连接超时
    WIFI_STATE_CONNECT_FAILED,      // 其他连接失败，不能判定为密码错误
} wifi_state_t;

#define WIFI_PASSWORD_MIN_LEN 8U
#define WIFI_PASSWORD_MAX_LEN 32U

/* 当前连接的 WiFi 信息。 */
typedef struct {
    char status[32];    // 连接状态：COMPLETED / DISCONNECTED
    char ssid[33];      // WiFi 名称，支持 32 字节 SSID
    char ip[32];        // 本机 IP
    char freq[16];      // 频率（字符串）
    char mac[32];       // 本机 WiFi MAC
    char dns1[32];      // 主 DNS 地址
    char auth[16];      // 当前网络加密方式
    int rssi;           // 当前网络信号强度
} wifi_info_t;

/* 扫描到的单个 WiFi 信息。 */
#define MAX_SCAN_AP_COUNT  32
typedef struct {
    char    bssid[32];      // 路由器MAC
    char    ssid[33];       // WiFi名称
    char    freq[16];       // 信道频率
    int     rssi;           // 信号强度
    char    auth[16];       // 加密方式
    bool    saved;          // 后台已保存该网络密码
} wifi_scan_ap_t;

typedef struct {
    wifi_state_t        wifi_state;                         // target_ssid 的连接状态/结果
    wifi_info_t         connected_info;                     // 当前连接网络信息
    int                 ap_num;                             // 扫描到的 AP 数量
    wifi_scan_ap_t      ap_list[MAX_SCAN_AP_COUNT];         // 扫描到的 AP 列表
    uint32_t            publish_sequence;                   // 快照发布序号
    uint32_t            scan_sequence;                      // 完整扫描完成序号
    bool                scanning;                          // 驱动正在扫描
    bool                scan_pending;                      // 扫描请求正在等待工作线程处理
    int                 scan_error;                        // 最近一次扫描结果，独立于连接和保存错误
    int                 error;                             // 最近操作结果，0 成功
    char                target_ssid[33];                    // 本次连接目标
    uint32_t            connection_id;                      // 连接请求代次，区别同一热点的不同尝试
} wifi_scan_result_t;

#ifdef __cplusplus
}
#endif

#endif
