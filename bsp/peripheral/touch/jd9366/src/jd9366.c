/*
 * Copyright (c) 2025, ArtInChip Technology Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date              Notes
 * 2025-07-03        The first version
 * 2025-07-04        Need to change touch.c c`s falling edge trigger to low level trigger
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <string.h>
#include "jd9366.h"
#include "touch_common.h"
#include <stdio.h>
#define DBG_TAG     AIC_TOUCH_PANEL_NAME
#define DBG_LVL     DBG_INFO
#include <rtdbg.h>

#define JD9366TS_I2C_CMD_LEN                    6
#define JD9366TS_I2C_RETRY                      3
#define JD9366TS_I2C_RETRY_DELAY_MS             20

#define JD9365TX_ID                             0x9084
#define JD9365TX_MEMORY_ADDR_ERAM               0x20011000
#define JD9365TX_MEMORY_ERAM_SIZE               (4 * 1024)
#define JD9365TX_SECTION_INFO_READY_VALUE       0xA55A
#define JD9365TX_MAX_DSRAM_NUM                  25
#define JD9365TX_MAX_ESRAM_NUM                  10
#define JD9365TX_ESRAM_COORDINATE_REPORT        1

#define JD9365TX_SOC_BASE_ADDR                  0x400080
#define JD9365TX_SOC_REG_ADDR_CHIP_ID2          ((JD9365TX_SOC_BASE_ADDR << 8) + 0x76)

#define JD9366TS_TOUCH_DATA_SIZE                78
#define JD9366TS_TOUCH_STYLUS_SIZE              14
#define JD9366TS_TOUCH_READ_LEN                 (JD9366TS_TOUCH_DATA_SIZE - JD9366TS_TOUCH_STYLUS_SIZE)
#define JD9366TS_FINGER_DATA_SIZE               5
#define JD9366TS_TOUCH_COORD_INFO_ADDR          3
#define JD9366TS_DEFAULT_COORDINATE_REPORT      0x20021120

static int16_t g_pre_x[JD9366TS_MAX_TOUCH] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
static int16_t g_pre_y[JD9366TS_MAX_TOUCH] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
static rt_uint8_t g_s_tp_dowm[JD9366TS_MAX_TOUCH] = {0};
static struct rt_touch_data *g_read_data = RT_NULL;
static struct rt_i2c_client g_jd9366ts_client = {0};
static rt_uint32_t g_coordinate_report_addr = JD9366TS_DEFAULT_COORDINATE_REPORT;
static rt_uint16_t g_coordinate_report_len = JD9366TS_TOUCH_READ_LEN;

static rt_uint32_t jd9366ts_le32(const rt_uint8_t *p)
{
    return ((rt_uint32_t)p[0]) |
           ((rt_uint32_t)p[1] << 8) |
           ((rt_uint32_t)p[2] << 16) |
           ((rt_uint32_t)p[3] << 24);
}

static rt_err_t jd9366ts_i2c_write(struct rt_i2c_client *dev,
                                   rt_uint8_t *cmd, rt_uint8_t cmd_len,
                                   rt_uint8_t *data, rt_uint16_t data_len)
{
    int retry;
    rt_uint8_t stack_buf[16];
    rt_uint8_t *tx_buf = stack_buf;
    rt_uint16_t tx_len = cmd_len + data_len;
    struct rt_i2c_msg msg;

    if (!dev || !dev->bus || !cmd || !cmd_len)
        return -RT_EINVAL;

    if (tx_len > sizeof(stack_buf)) {
        tx_buf = rt_malloc(tx_len);
        if (!tx_buf)
            return -RT_ENOMEM;
    }

    rt_memcpy(tx_buf, cmd, cmd_len);
    if (data && data_len)
        rt_memcpy(tx_buf + cmd_len, data, data_len);

    for (retry = 0; retry < JD9366TS_I2C_RETRY; retry++) {
        msg.addr = dev->client_addr;
        msg.flags = RT_I2C_WR;
        msg.buf = tx_buf;
        msg.len = tx_len;

        if (rt_i2c_transfer(dev->bus, &msg, 1) == 1) {
            if (tx_buf != stack_buf)
                rt_free(tx_buf);
            return RT_EOK;
        }

        rt_thread_mdelay(JD9366TS_I2C_RETRY_DELAY_MS);
    }

    if (tx_buf != stack_buf)
        rt_free(tx_buf);

    LOG_E("JD9366 write fail");
    return -RT_ERROR;
}

static rt_err_t jd9366ts_i2c_read(struct rt_i2c_client *dev,
                                  rt_uint8_t *cmd, rt_uint8_t cmd_len,
                                  rt_uint8_t *data, rt_uint16_t len)
{
    int retry;
    struct rt_i2c_msg msgs[2] = {0};

    if (!dev || !dev->bus || !cmd || !cmd_len || !data || !len)
        return -RT_EINVAL;

    for (retry = 0; retry < JD9366TS_I2C_RETRY; retry++) {
        msgs[0].addr = dev->client_addr;
        msgs[0].flags = RT_I2C_WR;
        msgs[0].buf = cmd;
        msgs[0].len = cmd_len;

        msgs[1].addr = dev->client_addr;
        msgs[1].flags = RT_I2C_RD;
        msgs[1].buf = data;
        msgs[1].len = len;

        if (rt_i2c_transfer(dev->bus, msgs, 2) == 2)
            return RT_EOK;

        rt_thread_mdelay(JD9366TS_I2C_RETRY_DELAY_MS);
    }

    LOG_E("JD9366 read fail");
    return -RT_ERROR;
}

static rt_err_t jd9366ts_enter_backdoor(void)
{
    rt_uint8_t cmd[5] = {0xF2, 0xAA, 0xF0, 0x0F, 0x55};
    rt_uint8_t data[1] = {0x68};

    return jd9366ts_i2c_write(&g_jd9366ts_client, cmd, sizeof(cmd), data, sizeof(data));
}

static rt_err_t jd9366ts_exit_backdoor(void)
{
    rt_uint8_t cmd[5] = {0xF2, 0xAA, 0x88, 0x00, 0x00};
    rt_uint8_t data[1] = {0x00};

    return jd9366ts_i2c_write(&g_jd9366ts_client, cmd, sizeof(cmd), data, sizeof(data));
}

static rt_err_t jd9366ts_bd_read(rt_uint32_t addr, rt_uint8_t *data, rt_uint16_t len)
{
    rt_uint8_t cmd[JD9366TS_I2C_CMD_LEN];

    cmd[0] = 0xF3;
    cmd[1] = (rt_uint8_t)((addr >> 24) & 0xFF);
    cmd[2] = (rt_uint8_t)((addr >> 16) & 0xFF);
    cmd[3] = (rt_uint8_t)((addr >> 8) & 0xFF);
    cmd[4] = (rt_uint8_t)(addr & 0xFF);
    cmd[5] = 0x03;

    return jd9366ts_i2c_read(&g_jd9366ts_client, cmd, sizeof(cmd), data, len);
}

static rt_err_t jd9366ts_bd_write(rt_uint32_t addr, rt_uint8_t *data, rt_uint16_t len)
{
    rt_uint8_t cmd[JD9366TS_I2C_CMD_LEN];

    cmd[0] = 0xF2;
    cmd[1] = (rt_uint8_t)((addr >> 24) & 0xFF);
    cmd[2] = (rt_uint8_t)((addr >> 16) & 0xFF);
    cmd[3] = (rt_uint8_t)((addr >> 8) & 0xFF);
    cmd[4] = (rt_uint8_t)(addr & 0xFF);
    cmd[5] = 0x03;

    return jd9366ts_i2c_write(&g_jd9366ts_client, cmd, sizeof(cmd), data, len);
}

static rt_err_t jd9366ts_reg_read(rt_uint32_t addr, rt_uint8_t *data, rt_uint16_t len)
{
    rt_err_t ret;

    if ((addr >= JD9365TX_MEMORY_ADDR_ERAM) &&
        (addr < (JD9365TX_MEMORY_ADDR_ERAM + JD9365TX_MEMORY_ERAM_SIZE))) {
        return jd9366ts_bd_read(addr, data, len);
    }

    ret = jd9366ts_enter_backdoor();
    if (ret != RT_EOK)
        return ret;

    ret = jd9366ts_bd_read(addr, data, len);
    jd9366ts_exit_backdoor();

    return ret;
}

static rt_err_t jd9366ts_reg_write(rt_uint32_t addr, rt_uint8_t *data, rt_uint16_t len)
{
    rt_err_t ret;

    ret = jd9366ts_enter_backdoor();
    if (ret != RT_EOK)
        return ret;

    ret = jd9366ts_bd_write(addr, data, len);
    jd9366ts_exit_backdoor();

    return ret;
}

static rt_err_t jd9366ts_read_chip_id(void)
{
    rt_err_t ret;
    rt_uint8_t id_buf[2];
    rt_uint16_t chip_id;

    ret = jd9366ts_enter_backdoor();
    if (ret != RT_EOK)
        return ret;

    ret = jd9366ts_bd_read(JD9365TX_SOC_REG_ADDR_CHIP_ID2, id_buf, sizeof(id_buf));
    jd9366ts_exit_backdoor();

    if (ret != RT_EOK)
        return ret;

    chip_id = ((rt_uint16_t)id_buf[1] << 8) | id_buf[0];
    LOG_I("chip id raw=%02x %02x id=0x%04x", id_buf[0], id_buf[1], chip_id);

    if (chip_id != JD9365TX_ID)
        return -RT_ERROR;

    return RT_EOK;
}

static rt_err_t jd9366ts_wait_section_ready(void)
{
    int i;
    rt_uint8_t buf[2];
    rt_uint16_t ready;

    for (i = 0; i < 50; i++) {
        if (jd9366ts_reg_read(JD9365TX_MEMORY_ADDR_ERAM, buf, sizeof(buf)) == RT_EOK) {
            ready = ((rt_uint16_t)buf[1] << 8) | buf[0];
            if (ready == JD9365TX_SECTION_INFO_READY_VALUE)
                return RT_EOK;
        }

        rt_thread_mdelay(10);
    }

    return -RT_ERROR;
}

static rt_err_t jd9366ts_read_section_info(void)
{
    rt_err_t ret;
    rt_uint8_t raw[JD9365TX_MAX_ESRAM_NUM * 8];
    rt_uint32_t dsram_section_start;
    rt_uint32_t esram_num_start;
    rt_uint32_t esram_section_start;
    rt_uint32_t addr;
    rt_uint32_t len;
    int i;

    dsram_section_start = JD9365TX_MEMORY_ADDR_ERAM + 4;
    esram_num_start = dsram_section_start + JD9365TX_MAX_DSRAM_NUM * 8;
    esram_section_start = esram_num_start + 4;

    ret = jd9366ts_wait_section_ready();
    if (ret != RT_EOK) {
        LOG_W("section not ready, use default coordinate addr 0x%08x", g_coordinate_report_addr);
        return RT_EOK;
    }

    ret = jd9366ts_reg_read(esram_section_start, raw, sizeof(raw));
    if (ret != RT_EOK) {
        LOG_W("read section fail, use default coordinate addr 0x%08x", g_coordinate_report_addr);
        return RT_EOK;
    }

    for (i = 0; i < JD9365TX_MAX_ESRAM_NUM; i++) {
        addr = jd9366ts_le32(&raw[i * 8]);
        len = jd9366ts_le32(&raw[i * 8 + 4]);
        LOG_D("ESRAM[%d] addr=0x%08x len=%d", i, addr, len);
    }

    addr = jd9366ts_le32(&raw[JD9365TX_ESRAM_COORDINATE_REPORT * 8]);
    len = jd9366ts_le32(&raw[JD9365TX_ESRAM_COORDINATE_REPORT * 8 + 4]);

    if (addr && len && len <= JD9366TS_TOUCH_DATA_SIZE) {
        g_coordinate_report_addr = addr;
        g_coordinate_report_len = (rt_uint16_t)len;
    }

    LOG_I("coordinate report addr=0x%08x len=%d", g_coordinate_report_addr, g_coordinate_report_len);
    return RT_EOK;
}

static rt_err_t jd9366ts_check_packet(rt_uint8_t *buf, rt_uint16_t len)
{
    int i;
    rt_uint8_t all_zero = 1;
    rt_uint8_t all_ff = 1;
    rt_uint16_t checksum = 0;

    for (i = 0; i < len; i++) {
        if (buf[i] != 0x00)
            all_zero = 0;
        if (buf[i] != 0xFF)
            all_ff = 0;
    }

    if (all_zero || all_ff)
        return -RT_ERROR;

    if (len >= 59) {
        for (i = 0; i < 59; i++)
            checksum += buf[i];

        if ((checksum & 0xFF) != 0) {
            LOG_D("checksum fail 0x%04x", checksum);
            return -RT_ERROR;
        }
    }

    return RT_EOK;
}

static void jd9366ts_touch_up(void *buf, int8_t id)
{
    g_read_data = (struct rt_touch_data *)buf;

    if (g_s_tp_dowm[id] == 1) {
        g_s_tp_dowm[id] = 0;
        g_read_data[id].event = RT_TOUCH_EVENT_UP;
    } else {
        g_read_data[id].event = RT_TOUCH_EVENT_NONE;
    }

    g_read_data[id].timestamp = rt_touch_get_ts();
    g_read_data[id].track_id = id;

    if ((g_pre_x[id] >= 0) && (g_pre_y[id] >= 0)) {
        int16_t tx = g_pre_x[id];
        int16_t ty = g_pre_y[id];
        if (tx < 0) tx = 0;
        if (ty < 0) ty = 0;
        g_read_data[id].x_coordinate = tx;
        g_read_data[id].y_coordinate = ty;
    } else {
        g_read_data[id].x_coordinate = 0;
        g_read_data[id].y_coordinate = 0;
    }

    g_pre_x[id] = -1;
    g_pre_y[id] = -1;
}

static void jd9366ts_touch_down(void *buf, int8_t id, int16_t x, int16_t y)
{
    g_read_data = (struct rt_touch_data *)buf;

    if (g_s_tp_dowm[id] == 1) {
        g_read_data[id].event = RT_TOUCH_EVENT_MOVE;
    } else {
        g_read_data[id].event = RT_TOUCH_EVENT_DOWN;
        g_s_tp_dowm[id] = 1;
    }

    g_read_data[id].timestamp = rt_touch_get_ts();
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    g_read_data[id].x_coordinate = x;
    g_read_data[id].y_coordinate = y;
    g_read_data[id].track_id = id;

    g_pre_x[id] = x;
    g_pre_y[id] = y;
}

static rt_size_t jd9366ts_read_point(struct rt_touch_device *touch,
                                     void *buf, rt_size_t read_num)
{
    rt_uint8_t read_buf[JD9366TS_TOUCH_READ_LEN] = {0};
    rt_uint8_t max_touch;
    rt_uint8_t i;

    rt_memset(buf, 0, sizeof(struct rt_touch_data) * read_num);

    if (jd9366ts_reg_read(g_coordinate_report_addr, read_buf, sizeof(read_buf)) != RT_EOK) {
        LOG_D("read point failed");
        return 0;
    }

    if (jd9366ts_check_packet(read_buf, sizeof(read_buf)) != RT_EOK)
        return 0;

    max_touch = JD9366TS_MAX_TOUCH;
    if (max_touch > read_num)
        max_touch = read_num;

    for (i = 0; i < max_touch; i++) {
        rt_uint8_t *p = &read_buf[JD9366TS_TOUCH_COORD_INFO_ADDR + i * JD9366TS_FINGER_DATA_SIZE];
        int16_t input_x;
        int16_t input_y;

        input_x = ((rt_uint16_t)p[0] << 8) | p[1];
        input_y = ((rt_uint16_t)p[2] << 8) | p[3];

        if ((input_x >= 0) && (input_x <= AIC_TOUCH_X_COORDINATE_RANGE) &&
            (input_y >= 0) && (input_y <= AIC_TOUCH_Y_COORDINATE_RANGE)) {
            aic_touch_flip(&input_x, &input_y);
            aic_touch_rotate(&input_x, &input_y);

            aic_touch_scale(&input_x, &input_y);
            if (!aic_touch_crop(&input_x, &input_y)) {
                jd9366ts_touch_up(buf, i);
                continue;
            }

            jd9366ts_touch_down(buf, i, input_x, input_y);
        } else {
            jd9366ts_touch_up(buf, i);
        }
    }

    return read_num;
}

static rt_err_t jd9366ts_control(struct rt_touch_device *touch, int cmd, void *data)
{
    struct rt_touch_info *info = RT_NULL;

    switch(cmd)
    {
    case RT_TOUCH_CTRL_GET_ID:
        break;
    case RT_TOUCH_CTRL_GET_INFO:
        info = (struct rt_touch_info *)data;
        if (info == RT_NULL)
            return -RT_EINVAL;

        info->point_num = touch->info.point_num;
        info->range_x = touch->info.range_x;
        info->range_y = touch->info.range_y;
        info->type = touch->info.type;
        info->vendor = touch->info.vendor;
        break;
    case RT_TOUCH_CTRL_SET_MODE:
    case RT_TOUCH_CTRL_SET_X_RANGE:
    case RT_TOUCH_CTRL_SET_Y_RANGE:
    case RT_TOUCH_CTRL_SET_X_TO_Y:
    case RT_TOUCH_CTRL_DISABLE_INT:
    case RT_TOUCH_CTRL_ENABLE_INT:
    default:
        LOG_W("This cmd:%d is not support", cmd);
        return -RT_ERROR;
    }

    return RT_EOK;
}

static struct rt_touch_ops jd9366ts_touch_ops = {
    .touch_readpoint = jd9366ts_read_point,
    .touch_control = jd9366ts_control,
};

struct rt_touch_info jd9366ts_info =
{
    RT_TOUCH_TYPE_CAPACITANCE,
    RT_TOUCH_VENDOR_UNKNOWN,
    JD9366TS_MAX_TOUCH,
    (rt_int32_t)AIC_TOUCH_X_COORDINATE_RANGE,
    (rt_int32_t)AIC_TOUCH_Y_COORDINATE_RANGE,
};

static int jd9366ts_hw_init(const char *name, struct rt_touch_config *cfg)
{
    rt_err_t ret;
    struct rt_touch_device *touch_device = RT_NULL;

    touch_device = (struct rt_touch_device *)rt_malloc(sizeof(struct rt_touch_device));
    if (touch_device == RT_NULL) {
        LOG_E("touch device malloc fail");
        return -RT_ERROR;
    }
    rt_memset((void *)touch_device, 0, sizeof(struct rt_touch_device));

    g_jd9366ts_client.bus = (struct rt_i2c_bus_device *)rt_device_find(cfg->dev_name);
    if (g_jd9366ts_client.bus == RT_NULL) {
        LOG_E("Can't find %s device", cfg->dev_name);
        rt_free(touch_device);
        return -RT_ERROR;
    }

    if (rt_device_open((rt_device_t)g_jd9366ts_client.bus, RT_DEVICE_FLAG_RDWR) != RT_EOK) {
        LOG_E("open %s device failed", cfg->dev_name);
        rt_free(touch_device);
        return -RT_ERROR;
    }

    g_jd9366ts_client.client_addr = JD9366TS_SALVE_ADDR;

    rt_pin_mode(*(rt_uint8_t *)cfg->user_data, PIN_MODE_OUTPUT);
    rt_pin_write(*(rt_uint8_t *)cfg->user_data, PIN_LOW);
    rt_thread_mdelay(10);
    rt_pin_write(*(rt_uint8_t *)cfg->user_data, PIN_HIGH);
    rt_thread_mdelay(120);

    ret = jd9366ts_read_chip_id();
    if (ret != RT_EOK) {
        LOG_E("JD9365TX chip id check failed");
        rt_free(touch_device);
        return ret;
    }

    jd9366ts_read_section_info();

    touch_device->info = jd9366ts_info;
    rt_memcpy(&touch_device->config, cfg, sizeof(struct rt_touch_config));
    touch_device->ops = &jd9366ts_touch_ops;

    if (RT_EOK != rt_hw_touch_register(touch_device, name, RT_DEVICE_FLAG_INT_RX, RT_NULL)) {
        LOG_E("touch device jd9366ts init failed");
        rt_free(touch_device);
        return -RT_ERROR;
    }

    LOG_I("touch device jd9366ts init success");
    return RT_EOK;
}

static int rt_hw_jd9366ts_port(void)
{
    struct rt_touch_config cfg = {0};
    rt_uint8_t rst_pin = 0;

    rst_pin = rt_pin_get(AIC_TOUCH_PANEL_RST_PIN);
    cfg.dev_name = AIC_TOUCH_PANEL_I2C_CHAN;
    cfg.irq_pin.pin = rt_pin_get(AIC_TOUCH_PANEL_INT_PIN);
    cfg.irq_pin.mode = PIN_MODE_INPUT_PULLUP;
    cfg.user_data = &rst_pin;

    jd9366ts_hw_init(AIC_TOUCH_PANEL_NAME, &cfg);

    return 0;
}

INIT_DEVICE_EXPORT(rt_hw_jd9366ts_port);
