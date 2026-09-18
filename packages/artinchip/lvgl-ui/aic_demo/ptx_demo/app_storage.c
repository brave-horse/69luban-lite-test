#include "app_storage.h"
#include "storage.h"

#if 1
// #include <aic_osal.h>
#include <stdint.h>
#include <string.h>

#define STORAGE_SETUP_PATH "/data/setup.bin"
#define STORAGE_FLAGS_PATH "/data/flags.bin"
#define STORAGE_PASSWORD_PATH "/data/password.bin"
#define STORAGE_ROTATION_PATH "/data/screen_rot.bin"

#define APP_STORAGE_FLAGS_MAGIC 0x41504647U /* "APFG" */
#define APP_STORAGE_FLAGS_VERSION 1U

/* 除 Wi-Fi 密码外的轻量标志集中保存在同一个文件。屏幕旋转暂不改动。 */
typedef struct
{
    uint32_t magic;                /* 魔数，用于校验文件有效性 "APFG" */
    uint16_t version;              /* 文件格式版本号 */
    uint8_t wifi_enabled;          /* WiFi 开关状态（0/1） */
    // uint8_t reserved[2];           /* 保留字段，对齐填充 */
    uint8_t screen_rot;            /* 屏幕旋转 （0/1） */
} app_storage_flags_t;

static app_storage_flags_t s_flags;

static bool is_skip_setup = false;
// static aicos_mutex_t s_cache_mutex;
static bool s_initialized;

static void storage_flags_default(void)
{
    memset(&s_flags, 0, sizeof(s_flags));
    s_flags.magic = APP_STORAGE_FLAGS_MAGIC;
    s_flags.version = APP_STORAGE_FLAGS_VERSION;
}

static bool storage_flags_valid(const app_storage_flags_t *flags)
{
    return flags->magic == APP_STORAGE_FLAGS_MAGIC &&
           flags->version == APP_STORAGE_FLAGS_VERSION &&
           flags->wifi_enabled <= 1U &&
           flags->screen_rot <= 1U;
}

static bool storage_cache_lock(void)
{
    // return s_cache_mutex && aicos_mutex_take(s_cache_mutex, AICOS_WAIT_FOREVER) == 0;
    return true;
}

static void storage_cache_unlock(void)
{
    // (void)aicos_mutex_give(s_cache_mutex);
}

/* 校验存储项和缓冲区，业务层不接触实际文件路径。 */
static const char *storage_path_get(storage_key_t key)
{
    if (key == SKIP_SETUP_KEY)
    {
        return STORAGE_SETUP_PATH;
    }
    if (key == STORAGE_FLAGS_KEY)
    {
        return STORAGE_FLAGS_PATH;
    }
    if (key == STORAGE_WIFI_CREDENTIALS)
    {
        return STORAGE_PASSWORD_PATH;
    }
    return NULL;
}

static bool storage_load_impl(uint8_t key, void *p_data, uint16_t len, bool (*load_cb)(void *p_data, uint16_t len))
{
    if (!p_data || (len == 0))
    {
        return false;
    }
    const char *path = storage_path_get(key);

    if (path && storage_load(path, p_data, len) == STORAGE_OK)
    {
        if (load_cb)
        {
            return load_cb(p_data, len);
        }
        return true;
    }
    return false;
}


static bool storage_save_impl(uint8_t key, const void *p_data, uint16_t len)
{
    if (!p_data || (len == 0))
    {
        return false;
    }
    const char *path = storage_path_get(key);

    if (path && storage_save(path, p_data, len) == STORAGE_OK)
    {
        return true;
    }
    return false;
}

/* 启动时只读取一次标志文件，后续读取全部命中 RAM 镜像。 */
bool app_storage_init(void)
{
    if (s_initialized)
    {
        return true;
    }
    if (!storage_init())
    {
        return false;
    }

    // if (!s_cache_mutex)
    // {
    //     s_cache_mutex = aicos_mutex_create();
    // }
    // if (!s_cache_mutex)
    // {
    //     return false;
    // }

    storage_flags_default();
    if (storage_load(STORAGE_FLAGS_PATH, &s_flags, sizeof(s_flags)) != STORAGE_OK || !storage_flags_valid(&s_flags))
    {
        storage_flags_default();
        if (storage_save(STORAGE_FLAGS_PATH, &s_flags, sizeof(s_flags)) != STORAGE_OK)
        {
            return false;
        }
    }

    storage_skip_setup_load();
    
    s_initialized = true;
    return true;
}

bool storage_wifi_credentials_load(void *p_data, uint16_t len)
{
    return storage_load_impl(STORAGE_WIFI_CREDENTIALS, p_data, len, NULL);
}

bool storage_wifi_credentials_save(const void *p_data, uint16_t len)
{
    return storage_save_impl(STORAGE_WIFI_CREDENTIALS, p_data, len);
}

/**
 * 跳过开机引导
 */
#define SKIP_SETUP_MAIG_NUMBER      (0x23456789)
static bool storage_skip_setup_load_cb(void *p_data, uint16_t len)
{
    uint32_t u32_skip = *((uint32_t *)p_data);
    storage_cache_lock();
    is_skip_setup = (u32_skip == SKIP_SETUP_MAIG_NUMBER);
    storage_cache_unlock();
    return is_skip_setup;
}

bool storage_skip_setup_load(void)
{
    uint32_t u32_skip = 0;

    if (!storage_load_impl(SKIP_SETUP_KEY, &u32_skip, sizeof(u32_skip), storage_skip_setup_load_cb))
    {
        return false;
    }

    return u32_skip == SKIP_SETUP_MAIG_NUMBER;
}

bool storage_skip_setup_get(void)
{
    return is_skip_setup;
}

void storage_skip_setup_set(bool skip)
{
    storage_cache_lock();
    is_skip_setup = skip;
    storage_cache_unlock();

    uint32_t u32_skip = skip ? SKIP_SETUP_MAIG_NUMBER : 0;
    storage_save_impl(SKIP_SETUP_KEY, &u32_skip, sizeof(u32_skip));
}
//-----------------------------------------------------

/**
 * wifi开关状态
 */
bool storage_wifi_enable_get(void)
{
    return s_flags.wifi_enabled > 0;
}

bool app_storage_wifi_enabled_load(bool *enabled)
{
    if (!enabled || !s_initialized || !storage_cache_lock())
    {
        return false;
    }
    *enabled = s_flags.wifi_enabled > 0;
    storage_cache_unlock();
    return true;
}

bool app_storage_wifi_enabled_save(bool enabled)
{
    if (!s_initialized || !storage_cache_lock())
    {
        return false;
    }
    uint8_t previous = s_flags.wifi_enabled;
    uint8_t requested = enabled ? 1U : 0U;
    if (previous == requested)
    {
        storage_cache_unlock();
        return true;
    }
    s_flags.wifi_enabled = requested;
    bool saved = storage_save_impl(STORAGE_FLAGS_KEY, &s_flags, sizeof(s_flags));
    if (!saved)
    {
        s_flags.wifi_enabled = previous;
    }
    storage_cache_unlock();
    return saved;
}

void storage_wifi_enable_set(bool enable)
{
    storage_cache_lock();
    s_flags.wifi_enabled = enable ? 0x01 : 0x00;
    storage_cache_unlock();

    storage_save_impl(STORAGE_FLAGS_KEY, &s_flags, sizeof(s_flags));
}

//-----------------------------------------------------
/**
 * 屏幕旋转
 */
bool storage_screen_rot_get(void)
{
    return s_flags.screen_rot > 0;
}

void storage_screen_rot_set(bool rot)
{
    storage_cache_lock();
    s_flags.screen_rot = rot ? 0x01 : 0x00;
    storage_cache_unlock();

    storage_save_impl(STORAGE_FLAGS_KEY, &s_flags, sizeof(s_flags));
}

//-----------------------------------------------------
#endif
