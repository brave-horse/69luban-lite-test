#ifndef APP_STORAGE_H
#define APP_STORAGE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 业务只使用逻辑键，文件路径和 RAM 缓存由 app_storage.c 管理。 */
typedef enum
{
    SKIP_SETUP_KEY = 0,
    STORAGE_FLAGS_KEY, 
    STORAGE_WIFI_CREDENTIALS,   /* WiFi 凭据（SSID 和密码） */
    STORAGE_COUNT               /* 存储项总数，用于数组大小 */
} storage_key_t;

#if 1

/* 在 app_init 中、启动业务线程前调用；重复调用不会重建文件锁。 */
bool app_storage_init(void);

/* 配置标志从内存镜像读取，不会在每次调用时访问 Flash。 */
bool storage_wifi_credentials_load(void *p_data, uint16_t len);

/* 保存变化后的数据；相同值不会重复写 Flash。 */
bool storage_wifi_credentials_save(const void *p_data, uint16_t len);

bool storage_skip_setup_load(void);
bool storage_skip_setup_get(void);
void storage_skip_setup_set(bool skip);

bool storage_wifi_enable_get(void);
void storage_wifi_enable_set(bool enable);

bool storage_screen_rot_get(void);
void storage_screen_rot_set(bool rot);

#else
#define app_storage_init()                      true
#define storage_wifi_credentials_load(data, size)       true
#define storage_wifi_credentials_save(data, size)       true

#define storage_skip_setup_load()       true
#define storage_skip_setup_get()        true
#define storage_skip_setup_set(skip)

#define storage_wifi_enable_get()        true
#define storage_wifi_enable_set(skip)

#define storage_screen_rot_get()        true
#define storage_screen_rot_set(skip)

#endif
#ifdef __cplusplus
}
#endif

#endif
