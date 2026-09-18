#include "storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#ifdef STORAGE

#include "aic_core.h"

#define STORAGE_LOG(...) rt_kprintf("[storage] " __VA_ARGS__)

static aicos_mutex_t m_storage_mutex;
static bool m_storage_mutex_ready;

/* 确保文件模块的全局互斥锁已经创建。 */
static bool storage_mutex_init(void)
{
    if (m_storage_mutex_ready)
    {
        return true;
    }

    m_storage_mutex = aicos_mutex_create();
    if (!m_storage_mutex)
    {
        STORAGE_LOG("mutex create failed\n");
        return false;
    }

    m_storage_mutex_ready = true;
    return true;
}

/* 在启动业务线程前创建文件锁，避免多个线程首次访问时重复创建。 */
bool storage_init(void)
{
    return storage_mutex_init();
}

/* 判断文件是否不存在，兼容标准库和 RT-Thread 的错误码符号。 */
static bool storage_file_missing(void)
{
    return errno == ENOENT || errno == -ENOENT;
}

/* 构造同目录下的辅助文件路径。
 * @file_path: 原文件路径
 * @suffix: 辅助文件后缀 */
static char *storage_side_path(const char *file_path, const char *suffix)
{
    size_t length = strlen(file_path);
    size_t suffix_size = strlen(suffix) + 1U;
    char *path;

    /* 检查路径长度并为结尾零保留空间。 */
    if (length > SIZE_MAX - suffix_size) {
        return NULL;
    }
    path = malloc(length + suffix_size);
    if (path)
    {
        memcpy(path, file_path, length);
        memcpy(path + length, suffix, suffix_size);
    }
    return path;
}

/* 获取文件模块互斥锁。 */
static bool storage_lock(void)
{
    if (!storage_mutex_init())
    {
        return false;
    }

    if (aicos_mutex_take(m_storage_mutex, AICOS_WAIT_FOREVER) != 0)
    {
        STORAGE_LOG("mutex take failed\n");
        return false;
    }

    return true;
}

/* 释放文件模块互斥锁。 */
static void storage_unlock(void)
{
    (void)aicos_mutex_give(m_storage_mutex);
}

/* 计算当前模块使用的 CRC32。
 * @data: 数据首地址
 * @size: 数据长度
 * @sequence: 保存序号，零保持旧版校验结果；序号作为初值参与校验 */
static uint32_t storage_crc32(const void *data, size_t size, uint32_t sequence)
{
    static const uint32_t rtable[16] = {
        0x00000000,
        0x1DB71064,
        0x3B6E20C8,
        0x26D930AC,
        0x76DC4190,
        0x6B6B51F4,
        0x4DB26158,
        0x5005713C,
        0xEDB88320,
        0xF00F9344,
        0xD6D6A3E8,
        0xCB61B38C,
        0x9B64C2B0,
        0x86D3D2D4,
        0xA00AE278,
        0xBDBDF21C,
    };
    const uint8_t *bytes = data;
    uint32_t crc = ~sequence;
    size_t index;

    if (!data && (size != 0U))
    {
        return 0U;
    }

    for (index = 0U; index < size; index++)
    {
        crc = (crc >> 4) ^ rtable[(crc ^ bytes[index]) & 0x0FU];
        crc = (crc >> 4) ^ rtable[(crc ^ (bytes[index] >> 4)) & 0x0FU];
    }

    return crc;
}

/* 返回文件操作结果的文字，便于统一输出日志。 */
/* @result: 文件操作结果 */
static const char *storage_result_name(storage_result_t result)
{
    switch (result)
    {
    case STORAGE_OK:
        return "ok";
    case STORAGE_INVALID_PARAM:
        return "invalid-param";
    case STORAGE_MUTEX_FAILED:
        return "mutex-failed";
    case STORAGE_OPEN_FAILED:
        return "open-failed";
    case STORAGE_READ_FAILED:
        return "read-failed";
    case STORAGE_WRITE_FAILED:
        return "write-failed";
    case STORAGE_CRC_FAILED:
        return "crc-failed";
    case STORAGE_VERIFY_FAILED:
        return "verify-failed";
    case STORAGE_MEMORY_FAILED:
        return "memory-failed";
    case STORAGE_NOT_FOUND:
        return "not-found";
    default:
        return "unknown";
    }
}

/* 在已经持有文件锁的情况下读取并校验文件。 */
/* @file_path: 文件路径
 * @data: 输出数据缓冲区
 * @size: 期望读取的数据长度
 * @sequence: 输出保存序号，旧版文件为零 */
static storage_result_t storage_load_locked(const char *file_path, void *data, size_t size, uint32_t *sequence)
{
    FILE *file;
    uint32_t stored_crc = 0U;
    uint32_t calculated_crc;
    size_t read_size;
    int extra_byte;
    storage_result_t result;

    /* 参数合法性检查 */
    if (!file_path || (!data && (size != 0U)))
    {
        return STORAGE_INVALID_PARAM;
    }

    /* 打开文件并读取数据内容 */
    file = fopen(file_path, "rb");
    if (!file)
    {
        result = storage_file_missing() ? STORAGE_NOT_FOUND : STORAGE_OPEN_FAILED;
        STORAGE_LOG("read path=%s result=%s\n", file_path, storage_result_name(result));
        return result;
    }

    read_size = (size != 0U) ? fread(data, 1U, size, file) : 0U;
    if (read_size != size)
    {
        (void)fclose(file);
        if (data && (size != 0U))
        {
            memset(data, 0, size);
        }
        STORAGE_LOG("read path=%s size=%lu actual=%lu result=%s\n",
                    file_path, (unsigned long)size,
                    (unsigned long)read_size,
                    storage_result_name(STORAGE_READ_FAILED));
        return STORAGE_READ_FAILED;
    }

    /* 读取文件末尾保存的 CRC32 */
    if (fread(&stored_crc, 1U, sizeof(stored_crc), file) !=
        sizeof(stored_crc))
    {
        (void)fclose(file);
        if (data && (size != 0U))
        {
            memset(data, 0, size);
        }
        STORAGE_LOG("read path=%s missing crc result=%s\n",
                    file_path,
                    storage_result_name(STORAGE_READ_FAILED));
        return STORAGE_READ_FAILED;
    }

    /* CRC 后增加序号；旧文件没有序号，按零处理。 */
    *sequence = 0U;
    read_size = fread(sequence, 1U, sizeof(*sequence), file);
    if (read_size != 0U && read_size != sizeof(*sequence))
    {
        (void)fclose(file);
        if (data && size) {
            memset(data, 0, size);
        }
        return STORAGE_READ_FAILED;
    }

    /* 拒绝序号后仍有多余数据的文件 */
    extra_byte = fgetc(file);
    if ((extra_byte != EOF) || ferror(file))
    {
        (void)fclose(file);
        if (data && (size != 0U))
        {
            memset(data, 0, size);
        }
        STORAGE_LOG("read path=%s extra data result=%s\n",
                    file_path,
                    storage_result_name(STORAGE_READ_FAILED));
        return STORAGE_READ_FAILED;
    }
    (void)fclose(file);

    /* 计算 CRC 并与文件中的 CRC 比较 */
    calculated_crc = storage_crc32(data, size, *sequence);
    STORAGE_LOG("read path=%s size=%lu stored_crc=0x%08lx calculated_crc=0x%08lx\n",
                file_path, (unsigned long)size,
                (unsigned long)stored_crc,
                (unsigned long)calculated_crc);
    if (calculated_crc != stored_crc)
    {
        if (data && (size != 0U))
        {
            memset(data, 0, size);
        }
        result = STORAGE_CRC_FAILED;
        STORAGE_LOG("read path=%s result=%s\n",
                    file_path, storage_result_name(result));
        return result;
    }

    STORAGE_LOG("read path=%s result=%s\n",
                file_path, storage_result_name(STORAGE_OK));
    return STORAGE_OK;
}

/* 写入指定槽文件并同步到存储设备。
 * @file_path: 槽文件路径
 * @data: 待写入数据首地址
 * @size: 待写入数据长度
 * @sequence: 本次保存序号 */
static storage_result_t storage_write_file(const char *file_path, const void *data, size_t size, uint32_t sequence)
{
    FILE *file;
    uint32_t calculated_crc;
    size_t write_size;
    int close_result;
    storage_result_t result;

    /* 参数合法性检查 */
    if (!file_path || (!data && (size != 0U)))
    {
        return STORAGE_INVALID_PARAM;
    }

    /* 计算待保存数据的 CRC32 */
    calculated_crc = storage_crc32(data, size, sequence);
    STORAGE_LOG("write path=%s size=%lu calculated_crc=0x%08lx\n",
                file_path, (unsigned long)size,
                (unsigned long)calculated_crc);

    /* 写入数据和 CRC32。 */
    file = fopen(file_path, "wb");
    if (!file)
    {
        result = STORAGE_OPEN_FAILED;
        STORAGE_LOG("write path=%s result=%s\n",
                    file_path, storage_result_name(result));
        return result;
    }

    write_size = (size != 0U) ? fwrite(data, 1U, size, file) : 0U;
    if (write_size != size)
    {
        (void)fclose(file);
        result = STORAGE_WRITE_FAILED;
        STORAGE_LOG("write path=%s size=%lu actual=%lu result=%s\n",
                    file_path, (unsigned long)size,
                    (unsigned long)write_size,
                    storage_result_name(result));
        return result;
    }

    write_size = fwrite(&calculated_crc, 1U, sizeof(calculated_crc), file);
    /* 先清空标准库缓冲区，再要求文件系统同步数据和元数据。 */
    if (write_size != sizeof(calculated_crc) ||
        fwrite(&sequence, 1U, sizeof(sequence), file) != sizeof(sequence) ||
        fflush(file) != 0 ||
        fsync(fileno(file)) != 0)
    {
        (void)fclose(file);
        return STORAGE_WRITE_FAILED;
    }
    close_result = fclose(file);
    if (close_result != 0)
    {
        result = STORAGE_WRITE_FAILED;
        STORAGE_LOG("write path=%s result=%s\n",
                    file_path, storage_result_name(result));
        return result;
    }

    return STORAGE_OK;
}

/* 比较保存序号，允许 uint32_t 自然回绕。
 * @first: 待比较序号
 * @second: 参考序号 */
static bool storage_sequence_newer(uint32_t first, uint32_t second)
{
    uint32_t difference = first - second;
    return difference != 0U && difference < 0x80000000U;
}

/* 在文件锁保护下写入较旧或无效的槽，保留最新有效槽。
 * @file_path: A 槽路径，追加 .bak 为 B 槽
 * @data: 待写入数据首地址，调用返回前必须保持有效且不变
 * @size: 待写入数据长度 */
static storage_result_t storage_save_locked(const char *file_path, const void *data, size_t size)
{
    char *backup_path = storage_side_path(file_path, ".bak");
    void *verify_data = size ? malloc(size) : NULL;
    storage_result_t save_result = STORAGE_MEMORY_FAILED;
    storage_result_t slot_a_result;
    storage_result_t slot_b_result;
    uint32_t primary_sequence = 0U;
    uint32_t backup_sequence = 0U;
    const char *target_path = NULL;
    uint32_t target_sequence = 0U;

    /* 提前分配全部缓冲区，内存不足时不触碰已有文件。 */
    if (!backup_path || (size && !verify_data)) {
        goto done;
    }

    /* 先分别检查 A、B 两个槽。 */
    slot_a_result = storage_load_locked(file_path, verify_data, size, &primary_sequence);
    slot_b_result = storage_load_locked(backup_path, verify_data, size, &backup_sequence);

    /* A 槽有效且更新时，写入 B 槽。 */
    if (slot_a_result == STORAGE_OK &&
        (slot_b_result != STORAGE_OK ||
         !storage_sequence_newer(backup_sequence, primary_sequence)))
    {
        target_path = backup_path;
        target_sequence = primary_sequence + 1U;
    }
    /* B 槽有效且更新时，写入 A 槽。 */
    else if (slot_b_result == STORAGE_OK)
    {
        target_path = file_path;
        target_sequence = backup_sequence + 1U;
    }
    /* 两个槽都不存在时，首次写入 A 槽。 */
    else if (slot_a_result == STORAGE_NOT_FOUND && slot_b_result == STORAGE_NOT_FOUND)
    {
        target_path = file_path;
        target_sequence = 1U;
    }
    else
    {
        /* 两个槽都无效时返回更具体的错误，不覆盖现场。 */
        save_result = slot_a_result != STORAGE_NOT_FOUND ? slot_a_result : slot_b_result;
    }

    /* 只在确定目标槽后写入一次。 */
    if (target_path)
    {
        save_result = storage_write_file(target_path, data, size, target_sequence);
    }

done:
    free(verify_data);
    free(backup_path);
    STORAGE_LOG("write path=%s result=%s\n",
                file_path, storage_result_name(save_result));
    return save_result;
}

storage_result_t storage_load(const char *file_path, void *data, size_t size)
{
    storage_result_t result;

    /* 参数合法性检查 */
    if (!file_path || (!data && (size != 0U)))
    {
        STORAGE_LOG("read invalid parameter\n");
        return STORAGE_INVALID_PARAM;
    }

    /* 串行化文件读取和 CRC 校验 */
    if (!storage_lock())
    {
        return STORAGE_MUTEX_FAILED;
    }
    char *backup_path = storage_side_path(file_path, ".bak");
    void *backup_data = size ? malloc(size) : NULL;
    uint32_t primary_sequence = 0U;
    uint32_t backup_sequence = 0U;

    result = STORAGE_MEMORY_FAILED;
    if (backup_path && (!size || backup_data))
    {
        /* 两槽分别校验，只有 B 槽有效且更新时才替换输出数据。 */
        result = storage_load_locked(file_path, data, size, &primary_sequence);
        storage_result_t backup_result =
            storage_load_locked(backup_path, backup_data, size, &backup_sequence);
        if (backup_result == STORAGE_OK &&
            (result != STORAGE_OK || storage_sequence_newer(backup_sequence, primary_sequence)))
        {
            if (size) memcpy(data, backup_data, size);
            result = STORAGE_OK;
        }
        else if (result == STORAGE_NOT_FOUND)
        {
            result = backup_result;
        }
    }
    free(backup_data);
    free(backup_path);
    storage_unlock();

    return result;
}

storage_result_t storage_save(const char *file_path, const void *data, size_t size)
{
    storage_result_t result;

    /* 参数合法性检查 */
    if (!file_path || (!data && (size != 0U)))
    {
        STORAGE_LOG("write invalid parameter\n");
        return STORAGE_INVALID_PARAM;
    }

    /* 串行化选槽、CRC 校验和文件写入 */
    if (!storage_lock())
    {
        return STORAGE_MUTEX_FAILED;
    }
    result = storage_save_locked(file_path, data, size);
    storage_unlock();

    return result;
}
#endif
