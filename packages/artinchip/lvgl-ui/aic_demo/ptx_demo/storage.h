#ifndef STORAGE_H
#define STORAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 槽文件格式：[data 数据][4 字节 CRC32][4 字节序号]；兼容无序号的旧文件。 */

typedef enum
{
    STORAGE_OK = 0,             // 操作成功
    STORAGE_INVALID_PARAM,      // 参数无效
    STORAGE_MUTEX_FAILED,       // 文件锁获取失败
    STORAGE_OPEN_FAILED,        // 文件打开失败
    STORAGE_READ_FAILED,        // 文件读取失败
    STORAGE_WRITE_FAILED,       // 文件写入失败
    STORAGE_CRC_FAILED,         // CRC 校验失败
    STORAGE_VERIFY_FAILED,      // 写入回读校验失败
    STORAGE_MEMORY_FAILED,      // 校验缓冲区分配失败
    STORAGE_NOT_FOUND,          // 文件不存在
} storage_result_t;


#define STORAGE
#ifdef STORAGE

/* 在启动业务线程前初始化文件锁，可重复调用。 */
bool storage_init(void);

/* 正式文件与 .bak 为固定 A/B 两槽，读取序号最新且 CRC32 有效的数据。
 * @file_path: 文件路径
 * @data: 输出数据缓冲区
 * @size: 期望读取的数据长度
 * @return: 文件读取和 CRC 校验结果 */
storage_result_t storage_load(const char *file_path,
                              void *data,
                              size_t size);

/* 校验 A/B 两槽，将新数据及递增序号同步写入较旧或无效的一槽。
 * 调用期间输入数据必须保持有效且不变；同一路径必须通过本模块访问。
 * 掉电恢复依赖文件系统及设备正确完成同步，不修复分区级损坏。
 * @file_path: 文件路径
 * @data: 待写入数据首地址
 * @size: 待写入数据长度
 * @return: 选槽校验和文件写入结果 */
storage_result_t storage_save(const char *file_path,
                              const void *data,
                              size_t size);

#else
#define storage_init()                            true
#define storage_load(file_path, data, size)         STORAGE_OK
#define storage_save(file_path, data, size)         STORAGE_OK
#endif
#ifdef __cplusplus
}
#endif

#endif /* STORAGE_H */
