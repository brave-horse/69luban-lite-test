#include "wifi_credentials.h"
#include "wifi_types.h"
#include "app_storage.h"
#include <stdint.h>
#include <string.h>

#define WIFI_PASSWORD_COUNT 8

/* 沿用已有文件布局，SSID 固定 32 字节，比较时使用有界长度。 */
typedef struct
{
    char ssid[32];
    char password[64];
    uint32_t last_used;
} wifi_password_t;

typedef struct
{
    wifi_password_t item[WIFI_PASSWORD_COUNT];
    uint32_t next_order;
} wifi_password_store_t;

static wifi_password_store_t m_store;

/* 比较凭据项中的 SSID 是否匹配目标 SSID。 */
static bool wifi_password_matches(const wifi_password_t *item, const char *ssid)
{
    size_t length = 0;
    while (length < sizeof(item->ssid) && item->ssid[length]) length++;
    return length == strlen(ssid) && !memcmp(item->ssid, ssid, length);
}

/* 从持久化存储加载 Wi-Fi 凭据并清理无效记录。 */
void wifi_credentials_load(void)
{
    memset(&m_store, 0, sizeof(m_store));
    if (!storage_wifi_credentials_load(&m_store, sizeof(m_store))) {
        memset(&m_store, 0, sizeof(m_store));
    }
    for (int i = 0; i < WIFI_PASSWORD_COUNT; i++)
    {
        if (!memchr(m_store.item[i].password, 0, sizeof(m_store.item[i].password))) {
            memset(&m_store.item[i], 0, sizeof(m_store.item[i]));
        }
    }
}

/* 按 SSID 查找已保存的 Wi-Fi 密码。 */
const char *wifi_credentials_find(const char *ssid)
{
    if (!ssid || !ssid[0]) {
        return NULL;
    }
    for (int i = 0; i < WIFI_PASSWORD_COUNT; i++)
    {
        if (wifi_password_matches(&m_store.item[i], ssid) &&
            strlen(m_store.item[i].password) <= WIFI_PASSWORD_MAX_LEN) {
            return m_store.item[i].password;
        }
    }
    return NULL;
}

/* 保存或更新 Wi-Fi 密码，写入失败时恢复修改前的内存记录。
 * @ssid: 网络名称
 * @password: 网络密码 */
bool wifi_credentials_save(const char *ssid, const char *password)
{
    if (!ssid || !password || !ssid[0] || strlen(ssid) > 32 ||
        strlen(password) > WIFI_PASSWORD_MAX_LEN ||
        (password[0] && strlen(password) < WIFI_PASSWORD_MIN_LEN)) {
        return false;
    }
    /* 先暂存输入，允许密码来自 wifi_credentials_find 返回的缓存地址。 */
    wifi_password_t updated = {0};
    memcpy(updated.ssid, ssid, strlen(ssid));
    memcpy(updated.password, password, strlen(password));

    /* 查找同名记录，其次使用空位，表满时替换最旧记录。 */
    wifi_password_t *item = NULL;
    wifi_password_t *oldest = &m_store.item[0];
    for (int i = 0; i < WIFI_PASSWORD_COUNT; i++)
    {
        wifi_password_t *candidate = &m_store.item[i];
        if (wifi_password_matches(candidate, ssid)) { item = candidate; break; }
        if (!candidate->ssid[0] && !item) item = candidate;
        if (candidate->last_used < oldest->last_used) oldest = candidate;
    }
    if (!item) item = oldest;
    /* 只备份本次修改的记录；序号溢出时还需保留各记录的旧序号。 */
    wifi_password_t previous;
    memcpy(&previous, item, sizeof(previous));
    uint32_t previous_order = m_store.next_order;
    uint32_t previous_used[WIFI_PASSWORD_COUNT];
    if (previous_order == UINT32_MAX)
    {
        for (int i = 0; i < WIFI_PASSWORD_COUNT; i++)
        {
            previous_used[i] = m_store.item[i].last_used;
            m_store.item[i].last_used = 0;
        }
        m_store.next_order = 0;
    }
    updated.last_used = ++m_store.next_order;
    memcpy(item, &updated, sizeof(*item));
    if (storage_wifi_credentials_save(&m_store, sizeof(m_store))) {
        return true;
    }

    /* 写入失败只恢复内存，不代表已恢复磁盘文件。 */
    memcpy(item, &previous, sizeof(*item));
    m_store.next_order = previous_order;
    if (previous_order == UINT32_MAX)
    {
        for (int i = 0; i < WIFI_PASSWORD_COUNT; i++) {
            m_store.item[i].last_used = previous_used[i];
        }
    }
    return false;
}

/* 删除 Wi-Fi 凭据，写入失败时恢复内存中的原记录。
 * @ssid: 要删除的网络名称 */
bool wifi_credentials_remove(const char *ssid)
{
    if (!ssid || !ssid[0])
    {
        return false;
    }

    for (int i = 0; i < WIFI_PASSWORD_COUNT; i++)
    {
        if (!wifi_password_matches(&m_store.item[i], ssid))
        {
            continue;
        }

        /* 直接清除目标记录，仅保留这一条旧值供写入失败时恢复。 */
        wifi_password_t previous;
        memcpy(&previous, &m_store.item[i], sizeof(previous));
        memset(&m_store.item[i], 0, sizeof(m_store.item[i]));
        if (storage_wifi_credentials_save(&m_store, sizeof(m_store)))
        {
            return true;
        }

        memcpy(&m_store.item[i], &previous, sizeof(m_store.item[i]));
        return false;
    }

    /* 目标凭据不存在，删除结果已经满足。 */
    return true;
}
