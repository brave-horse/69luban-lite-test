/*
 * Copyright (C) 2024-2026, ArtInChip Technology Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:  Ning Fang <ning.fang@artinchip.com>
 */

#include <pthread.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>

#include "lvgl.h"
#include "aic_core.h"
#include "mpp_mem.h"
#include "mpp_ge.h"
#include "lv_mpp_dec.h"
#include "lv_aic_stream.h"
#include "lv_aic_bmp.h"
#include "aic_ui.h"
#include "lv_draw_ge2d.h"
#include "lv_draw_ge2d_utils.h"

#define PNG_HEADER_SIZE (8 + 12 + 13) // PNG 文件签名和 IHDR 块长度
#define PNGSIG 0x89504e470d0a1a0aull
#define MNGSIG 0x8a4d4e470d0a1a0aull
#define JPEG_SOI 0xFFD8
#define JPEG_SOF 0xFFC0
#define AICP_SOF 0xFFC1
#define ALIGN_16B(x) (((x) + (15)) & ~(15))

/* 将 MPP 像素格式转换为 LVGL 像素格式。 */
lv_color_format_t mpp_fmt_to_lv_fmt(enum mpp_pixel_format cf)
{
    lv_color_format_t fmt = LV_COLOR_FORMAT_ARGB8888;

    switch(cf) {
        case MPP_FMT_RGB_565:
            fmt = LV_COLOR_FORMAT_RGB565;
            break;
        case MPP_FMT_RGB_888:
            fmt = LV_COLOR_FORMAT_RGB888;
            break;
        case MPP_FMT_ARGB_8888:
            fmt = LV_COLOR_FORMAT_ARGB8888;
            break;
        case MPP_FMT_XRGB_8888:
            fmt = LV_COLOR_FORMAT_XRGB8888;
            break;
        case MPP_FMT_YUV420P:
            fmt = LV_COLOR_FORMAT_I420;
            break;
        case MPP_FMT_NV12:
            fmt = LV_COLOR_FORMAT_NV12;
            break;
        case MPP_FMT_NV21:
            fmt = LV_COLOR_FORMAT_NV21;
            break;
        case MPP_FMT_YUV422P:
            fmt = LV_COLOR_FORMAT_I422;
            break;
        case MPP_FMT_YUV444P:
            fmt = LV_COLOR_FORMAT_I444;
            break;
        case MPP_FMT_YUV400:
            fmt = LV_COLOR_FORMAT_I400;
            break;
        default:
            LV_LOG_ERROR("unsupported format:%d", cf);
            break;
    }

    return fmt;
}

/* 按 MPP 像素格式计算 16 字节对齐的 RGB 行跨度。 */
static int mpp_get_rgb_stride(int width, enum mpp_pixel_format fmt)
{
    int stride;

    switch(fmt) {
        case MPP_FMT_RGB_565:
            stride = ALIGN_16B(width) * 2;
            break;
        case MPP_FMT_RGB_888:
            stride = ALIGN_16B(width) * 3;
            break;
        case MPP_FMT_ARGB_8888:
            stride = ALIGN_16B(width) * 4;
            break;
        case MPP_FMT_XRGB_8888:
            stride = ALIGN_16B(width) * 4;
            break;
        default:
            stride = ALIGN_16B(width) * 4;
            LV_LOG_ERROR("unsupported format:%d", fmt);
            break;
    }
    return stride;
}

/* 根据 JPEG SOF 采样因子确定解码输出格式。 */
static int get_jpeg_format(uint8_t *buf, enum mpp_pixel_format *pix_fmt)
{
    int i;
    uint32_t h_count_flag;
    uint32_t v_count_flag;
    uint8_t h_count[3] = { 0 };
    uint8_t v_count[3] = { 0 };
    uint8_t nb_components = *buf++;

    for (i = 0; i < nb_components; i++) {
        uint8_t h_v_cnt;

        // 跳过分量标识。
        buf++;
        h_v_cnt = *buf++;
        h_count[i] = h_v_cnt >> 4;
        v_count[i] = h_v_cnt & 0xf;

        // 跳过量化表索引。
        buf++;
    }

    h_count_flag =  h_count[2] | (h_count[1] << 4) | (h_count[0] << 8);
    v_count_flag =  v_count[2] | (v_count[1] << 4) | (v_count[0] << 8);

    if (h_count_flag == 0x211 && v_count_flag == 0x211) {
        *pix_fmt = MPP_FMT_YUV420P;
    } else if (h_count_flag == 0x211 && v_count_flag == 0x111) {
        *pix_fmt = MPP_FMT_YUV422P;
    } else if (h_count_flag == 0x111 && v_count_flag == 0x111) {
        *pix_fmt = MPP_FMT_YUV444P;
    } else if (h_count_flag == 0x111 && v_count_flag == 0x222) {
        *pix_fmt = MPP_FMT_YUV444P;
    } else if (h_count[1] == 0 && v_count[1] == 0 && h_count[2] == 0 &&
               v_count[2] == 0) {
        *pix_fmt = MPP_FMT_YUV400;
    } else {
        LV_LOG_ERROR("Not support format! h_count: %d %d %d, v_count: %d %d %d",
            h_count[0], h_count[1], h_count[2],
            v_count[0], v_count[1], v_count[2]);
        return -1;
    }

#ifndef AIC_VE_DRV_V10
#if LV_COLOR_DEPTH  == 16
    *pix_fmt = MPP_FMT_RGB_565;
#else
    *pix_fmt = MPP_FMT_RGB_888;
#endif
#endif

#ifdef AIC_VE_DRV_V31
    if (nb_components == 4)
        *pix_fmt = MPP_FMT_ARGB_8888;
#else
    if (nb_components == 4) {
        LV_LOG_ERROR("Unsupported nb_components: %d", nb_components);
        return -1;
    }
#endif
    return 0;
}

/* 校验流起始位置是否为 JPEG SOI 标识。 */
static inline lv_result_t check_jpeg_soi(lv_stream_t *stream)
{
    uint32_t read_num;
    uint8_t buf[128];
    lv_fs_res_t fs_res;
    lv_result_t res = LV_RESULT_OK;

    // 读取 JPEG SOI 标识。
    fs_res = lv_aic_stream_read(stream, buf, 2, &read_num);
    if (fs_res != LV_FS_RES_OK || read_num != 2) {
        res = LV_RESULT_INVALID;
        goto read_err;
    }

    // 校验 SOI 字节。
    if (buf[0] != 0xff || buf[1] != 0xd8) {
        res = LV_RESULT_INVALID;
        goto read_err;
    }

    // 校验 SOI 数值。
    if (stream_to_u16(buf) != JPEG_SOI) {
        res = LV_RESULT_INVALID;
        goto read_err;
    }

read_err:
    return res;
}

/* 扫描 JPEG SOF 块并取得图像尺寸和采样格式。 */
static lv_result_t jpeg_get_img_size(lv_stream_t *stream, int *w, int *h, enum mpp_pixel_format *pix_fmt, bool is_aicp)
{
    uint32_t read_num;
    uint8_t buf[128];
    lv_fs_res_t fs_res;
    lv_result_t res = LV_RESULT_OK;

    if (check_jpeg_soi(stream) != LV_RESULT_OK) {
        res = LV_RESULT_INVALID;
        LV_LOG_ERROR("check jpeg soi failed");
        goto read_err;
    }

    // 查找 SOF 图像帧头。
    while (1) {
        int size;
        uint16_t marker;
        fs_res = lv_aic_stream_read(stream, buf, 4, &read_num);
        if (fs_res != LV_FS_RES_OK || read_num != 4) {
            res = LV_RESULT_INVALID;
            LV_LOG_ERROR("get chunk header failed");
            goto read_err;
        }

        marker = stream_to_u16(buf);
        if (marker == JPEG_SOF || (is_aicp && marker == AICP_SOF)) {
            fs_res = lv_aic_stream_read(stream, buf, 15, &read_num);
            if (fs_res != LV_FS_RES_OK) {
                res = LV_RESULT_INVALID;
                LV_LOG_ERROR("read chunk data failed");
                goto read_err;
            }

            *h = stream_to_u16(buf + 1);
            *w = stream_to_u16(buf + 3);

            if (get_jpeg_format(buf + 5, pix_fmt) < 0) {
                res = LV_RESULT_INVALID;
                LV_LOG_ERROR("get format failed");
                goto read_err;
            }
            break;
        } else {
            size = stream_to_u16(buf + 2);
             fs_res = lv_aic_stream_seek(stream, size - 2, SEEK_CUR);
            if (fs_res != LV_FS_RES_OK) {
                res = LV_RESULT_INVALID;
                LV_LOG_ERROR("read chunk data failed");
                goto read_err;
            }
        }
    }
read_err:
    return res;
}

/* 读取 JPEG 或 AICP 图像头信息。 */
lv_result_t lv_jpeg_decoder_info(const char *src, lv_image_header_t *header, uint32_t size, bool is_file, bool is_aicp)
{
    lv_fs_res_t res;
    int width;
    int height;
    enum mpp_pixel_format format = MPP_FMT_ARGB_8888;
    lv_stream_t stream;

    if (is_file)
        res = lv_aic_stream_open_file(&stream, src);
    else
       res = lv_aic_stream_open_buf(&stream, src, size);

    if (res != LV_FS_RES_OK)
        return LV_RESULT_INVALID;

#ifdef AIC_MPP_AICP_DEC_ENABLE
    // 跳过并校验 AICP 文件头。
    if (is_aicp) {
        uint8_t aicp_header[4];
        uint32_t read_num;
        res = lv_aic_stream_read(&stream, aicp_header, 4, &read_num);
        if (res != LV_FS_RES_OK || read_num != 4 || memcmp(aicp_header, "AICP", 4) != 0) {
            LV_LOG_ERROR("AICP header not found");
            lv_aic_stream_close(&stream);
            return LV_RESULT_INVALID;
        }
    }
#endif

    res = jpeg_get_img_size(&stream, &width, &height, &format, is_aicp);
    if (res != LV_RESULT_OK) {
        lv_aic_stream_close(&stream);
        LV_LOG_ERROR("get img size failed");
        return LV_RESULT_INVALID;
    }

#ifdef AIC_VE_DRV_V10
    /* D21x JPEG VE V1.0 固定输出 NV12，必须与硬件写入的帧布局一致。 */
    if (!is_aicp)
        format = MPP_FMT_NV12;
#endif

#if defined(MPP_JPEG_DEC_OUT_SIZE_LIMIT_ENABLE)
    if (!is_aicp) {
        int size_shift = jpeg_size_limit(width, height);
        width = width >> size_shift;
        height = height >> size_shift;
        header->reserved_2 = size_shift;
    }
#endif

    header->w = width;
    header->h = height;
    header->cf = mpp_fmt_to_lv_fmt(format);

    if (!lv_fmt_is_yuv(header->cf))
        header->stride = mpp_get_rgb_stride(width, format);

    LV_LOG_INFO("w:%d, h:%d, cf:%d", header->w, header->h, header->cf);
	lv_aic_stream_close(&stream);

    return LV_RESULT_OK;
}

/* 校验流起始位置是否为 PNG 或 MNG 文件签名。 */
static inline lv_result_t check_png_sig(lv_stream_t *stream)
{
    uint32_t read_num;
    unsigned char buf[64];
    uint64_t sig;

    lv_aic_stream_read(stream, buf, 8, &read_num);
    sig = stream_to_u64(buf);
    if (sig != PNGSIG && sig != MNGSIG) {
        return LV_RESULT_INVALID;
    }

    return LV_RESULT_OK;
}

/* 读取 PNG 图像头信息并设置 LVGL 输出格式。 */
lv_result_t lv_png_decoder_info(const char *src, lv_image_header_t *header, uint32_t size, bool is_file)
{
    lv_fs_res_t res;
    uint32_t read_num;
    uint8_t buf[64];
    lv_stream_t stream;
    int color_type;

    if (is_file)
        res = lv_aic_stream_open_file(&stream, src);
    else
       res = lv_aic_stream_open_buf(&stream, src, size);

    if (res != LV_FS_RES_OK) {
        LV_LOG_ERROR("open %s failed", src);
        return LV_RESULT_INVALID;
    }

    // 读取 PNG 文件签名和 IHDR 块。
    res = lv_aic_stream_read(&stream, buf, PNG_HEADER_SIZE, &read_num);
    if (res != LV_FS_RES_OK || read_num != PNG_HEADER_SIZE) {
        LV_LOG_ERROR("lv_aic_stream_read failed");
        lv_aic_stream_close(&stream);
        return LV_RESULT_INVALID;
    }

    // 校验文件签名。
    uint64_t sig = stream_to_u64(buf);
    if (sig != PNGSIG && sig != MNGSIG) {
        LV_LOG_WARN("Invalid PNG signature 0x%08llx.", (unsigned long long)sig);
        lv_aic_stream_close(&stream);
        return LV_RESULT_INVALID;
    }

    header->w = stream_to_u32(buf + 8 + 8);
    header->h = stream_to_u32(buf + 8 + 8 + 4);

    color_type = buf[8 + 8 + 8 + 1];
    if (color_type == 2) {
        header->cf = mpp_fmt_to_lv_fmt(MPP_FMT_RGB_888);
        header->stride = mpp_get_rgb_stride(header->w, MPP_FMT_RGB_888);
    } else {
        header->cf = mpp_fmt_to_lv_fmt(MPP_FMT_ARGB_8888);
        header->stride = mpp_get_rgb_stride(header->w, MPP_FMT_ARGB_8888);
    }

    LV_LOG_INFO("header->w:%d, header->h:%d, header->cf:%d", header->w, header->h, header->cf);
    lv_aic_stream_close(&stream);

    return LV_RESULT_OK;
}

/* 解析测试用 fake 图像描述字符串。 */
static lv_res_t fake_decoder_info(const void *src, lv_image_header_t *header)
{
    int width;
    int height;
    int alpha_en;
    unsigned int color;

    FAKE_IMAGE_PARSE((char *)src, &width, &height, &alpha_en, &color);
    header->w = width;
    header->h = height;
    header->stride = mpp_get_rgb_stride(header->w, MPP_FMT_ARGB_8888);
    header->cf = LV_COLOR_FORMAT_RAW;
    return LV_RES_OK;
}

/* 根据图片来源和扩展名选择解码器并读取图像头。 */
static lv_result_t lv_mpp_dec_info(lv_image_decoder_t *decoder, const void *src, lv_image_header_t *header)
{
    char* ptr = NULL;
    lv_result_t res = LV_RESULT_INVALID;

    LV_UNUSED(decoder);

    if (lv_image_src_get_type(src) == LV_IMAGE_SRC_FILE) {
        ptr = strrchr(src, '.');
        if (!ptr)
            return LV_RESULT_INVALID;
        if (!strcmp(ptr, ".png")) {
            return lv_png_decoder_info(src, header, 0, true);
        } else if (image_suffix_is_jpg(ptr)) {
            return lv_jpeg_decoder_info(src, header, 0, true, false);
#ifdef AIC_MPP_AICP_DEC_ENABLE
        } else if (image_suffix_is_aicp(ptr)) {
            return lv_jpeg_decoder_info(src, header, 0, true, true);
#endif
        } else if (!strcmp(ptr, ".fake")) {
            return fake_decoder_info(src, header);
#if LV_USE_AIC_BMP
        } else if (!strcmp(ptr, ".bmp") || !strcmp(ptr, ".BMP")) {
            return lv_bmp_decoder_info(src, header, 0, true);
#endif
        } else {
            LV_LOG_ERROR("unsupported image:%s", (uint8_t *)src);
            return LV_RESULT_INVALID;
        }
    } else if (lv_image_src_get_type(src) == LV_IMAGE_SRC_VARIABLE) {
        lv_color_format_t cf = ((lv_img_dsc_t *)src)->header.cf;
        char *data = (char *)((lv_img_dsc_t *)src)->data;
        uint32_t data_size = ((lv_img_dsc_t *)src)->data_size;

        if (cf == LV_COLOR_FORMAT_RAW || cf == LV_COLOR_FORMAT_RAW) {
            bool is_aicp_data = false;
#ifdef AIC_MPP_AICP_DEC_ENABLE
            // 检查变量数据是否带有 AICP 文件头。
            if (data_size >= 4 && memcmp(data, "AICP", 4) == 0) {
                is_aicp_data = true;
            }
#endif
            res = lv_jpeg_decoder_info(data, header, data_size, false, is_aicp_data);

            if (res != LV_RESULT_OK) {
                res = lv_png_decoder_info(data, header, data_size, false);
#if LV_USE_AIC_BMP
                if (res != LV_RESULT_OK) {
                    res = lv_bmp_decoder_info(data, header, data_size, false);
                }
#endif
            }
        }
    }

    return res;
}

struct ext_frame_allocator {
    struct frame_allocator base;
    struct mpp_frame* frame;
};

/* 将预先分配的 LVGL 帧缓冲区交给 MPP 解码器使用。 */
static int alloc_frame_buffer(struct frame_allocator *p, struct mpp_frame* frame,
                              int width, int height, enum mpp_pixel_format format)
{
    struct ext_frame_allocator* impl = (struct ext_frame_allocator*)p;

    memcpy(frame, impl->frame, sizeof(struct mpp_frame));
    return 0;
}

/* 外部帧缓冲区由 LVGL 统一释放，此处无需处理。 */
static int free_frame_buffer(struct frame_allocator *p, struct mpp_frame *frame)
{
    return 0;
}

/* 释放 MPP 外部帧缓冲区分配器对象。 */
static int close_allocator(struct frame_allocator *p)
{
    struct ext_frame_allocator* impl = (struct ext_frame_allocator*)p;

    free(impl);

    return 0;
}

static struct alloc_ops def_ops = {
    .alloc_frame_buffer = alloc_frame_buffer,
    .free_frame_buffer = free_frame_buffer,
    .close_allocator = close_allocator,
};

/* 创建引用指定帧缓冲区的 MPP 外部分配器。 */
struct frame_allocator* lv_open_allocator(struct mpp_frame* frame)
{
    struct ext_frame_allocator* impl = (struct ext_frame_allocator*)malloc(sizeof(struct ext_frame_allocator));

    if(impl == NULL) {
        return NULL;
    }

    memset(impl, 0, sizeof(struct ext_frame_allocator));

    impl->frame = frame;
    impl->base.ops = &def_ops;
    return &impl->base;
}

/* 释放解码结果占用的 CMA 帧缓冲区。 */
void lv_frame_buf_free(mpp_decoder_data_t *mpp_data)
{
    int i;
    for (i = 0; i < 3; i++) {
        if (mpp_data->data[i]) {
            aicos_free(MEM_CMA, (void*)mpp_data->data[i]);
            mpp_data->data[i] = NULL;
        }
    }
}

/* 为 MPP 解码输出分配 CMA 帧缓冲区并填充 LVGL 描述符。 */
lv_result_t lv_frame_buf_alloc(mpp_decoder_data_t *mpp_data, struct mpp_buf *alloc_buf,
                              int *size, uint32_t cf)
{
    int i;
    int data_size = 0;
    for (i = 0; i < 3; i++) {
        if (size[i]) {
            mpp_data->data[i] = (uint8_t *)aicos_malloc_try_cma(size[i] + CACHE_LINE_SIZE - 1);
            if (!mpp_data->data[i]) {
                goto alloc_error;
            } else {
                unsigned int align_addr = (unsigned int)ALIGN_UP((ulong)mpp_data->data[i], CACHE_LINE_SIZE);
                alloc_buf->phy_addr[i] = align_addr;
                aicos_dcache_clean_invalid_range((ulong *)(ulong)align_addr, ALIGN_UP(size[i], CACHE_LINE_SIZE));
                data_size += size[i];
            }
        }
    }

    mpp_data->decoded.header.w = alloc_buf->size.width;
    mpp_data->decoded.header.h = alloc_buf->size.height;
    mpp_data->decoded.header.cf = cf;
    mpp_data->decoded.header.magic = LV_IMAGE_HEADER_MAGIC;

    if (lv_fmt_is_yuv(cf)) {
        mpp_data->decoded.data =(uint8_t *)(&mpp_data->dec_buf);
        mpp_data->decoded.data_size = data_size;
        mpp_data->decoded.unaligned_data = (uint8_t *)(&mpp_data->dec_buf);
    } else {
        mpp_data->decoded.header.stride = alloc_buf->stride[0];
        mpp_data->decoded.data =(uint8_t *)((ulong)alloc_buf->phy_addr[0]);
        mpp_data->decoded.data_size = size[0];
        mpp_data->decoded.unaligned_data = mpp_data->data[0];
    }

    return LV_RESULT_OK;
alloc_error:
    lv_frame_buf_free(mpp_data);
    return LV_RESULT_INVALID;
}

/* 按像素格式、对齐和缩放比例计算各平面缓冲区大小。 */
void lv_set_frame_buf_size(struct mpp_frame *frame, int *buf_size, int size_shift)
{
    int height_align;
    int width = frame->buf.size.width;
    int height = frame->buf.size.height;

    if (size_shift > 0) {
        height_align = ALIGN_16B(ALIGN_16B(height) >> size_shift);
        frame->buf.size.width =  width >> size_shift;;
        frame->buf.size.height = height >> size_shift;
    } else {
        height_align = ALIGN_16B(height);
    }

    switch (frame->buf.format) {
    case MPP_FMT_NV12:
    case MPP_FMT_NV21:
        if (size_shift > 0)
            frame->buf.stride[0] = ALIGN_16B(ALIGN_16B(width) >> size_shift);
        else
            frame->buf.stride[0] = ALIGN_16B(width);

        frame->buf.stride[1] = frame->buf.stride[0];
        frame->buf.stride[2] = 0;
        buf_size[0] = frame->buf.stride[0] * height_align;
        buf_size[1] = frame->buf.stride[1] * (height_align >> 1);
        buf_size[2] = 0;
        break;
    case MPP_FMT_YUV420P:
        if (size_shift > 0)
            frame->buf.stride[0] =  ALIGN_16B(ALIGN_16B(width) >> size_shift);
        else
            frame->buf.stride[0] =  ALIGN_16B(width);

        frame->buf.stride[1] =  frame->buf.stride[0] >> 1;
        frame->buf.stride[2] =  frame->buf.stride[0] >> 1;
        buf_size[0] = frame->buf.stride[0] * height_align;
        buf_size[1] = frame->buf.stride[1] * (height_align >> 1);
        buf_size[2] = frame->buf.stride[2] * (height_align >> 1);
        break;
    case MPP_FMT_YUV422P:
        if (size_shift > 0)
            frame->buf.stride[0] =  ALIGN_16B(ALIGN_16B(width) >> size_shift);
        else
            frame->buf.stride[0] =  ALIGN_16B(width);

        frame->buf.stride[1] =  frame->buf.stride[0] >> 1;
        frame->buf.stride[2] =  frame->buf.stride[0] >> 1;
        buf_size[0] = frame->buf.stride[0] * height_align;
        buf_size[1] = frame->buf.stride[1] * height_align;
        buf_size[2] = frame->buf.stride[2] * height_align;
        break;
    case MPP_FMT_YUV444P:
        if (size_shift > 0)
            frame->buf.stride[0] =  ALIGN_16B(ALIGN_16B(width) >> size_shift);
        else
            frame->buf.stride[0] =  ALIGN_16B(width);

        frame->buf.stride[1] =  frame->buf.stride[0];
        frame->buf.stride[2] =  frame->buf.stride[0];
        buf_size[0] = frame->buf.stride[0] * height_align;
        buf_size[1] = frame->buf.stride[1] * height_align;
        buf_size[2] = frame->buf.stride[2] * height_align;
        break;
    case MPP_FMT_RGB_565:
        if (size_shift > 0)
            frame->buf.stride[0] =  ALIGN_16B(ALIGN_16B(width) >> size_shift) * 2;
        else
            frame->buf.stride[0] =  ALIGN_16B(width) * 2;

        buf_size[0] = frame->buf.stride[0] * height_align;
        break;
    case MPP_FMT_RGB_888:
        if (size_shift > 0)
            frame->buf.stride[0] =  ALIGN_16B(ALIGN_16B(width) >> size_shift) * 3;
        else
            frame->buf.stride[0] =  ALIGN_16B(width) * 3;

        buf_size[0] = frame->buf.stride[0] * height_align;
        break;
    case MPP_FMT_ARGB_8888:
        if (size_shift > 0) {
            frame->buf.stride[0] =  ALIGN_16B(ALIGN_16B(width) >> size_shift) * 4;
            buf_size[0] = frame->buf.stride[0] * height_align;
        } else {
            frame->buf.stride[0] =  ALIGN_16B(width * 4);
            buf_size[0] = ALIGN_UP(frame->buf.stride[0] * height, CACHE_LINE_SIZE);
        }
        break;
    default:
        LV_LOG_ERROR("unsupported format:%d", frame->buf.format);
        break;
    }
}

/* 使用 GE2D 将 JPEG 临时 YUV 帧转换为 LVGL 使用的 RGB 帧。 */
static lv_result_t lv_mpp_yuv_to_rgb(mpp_decoder_data_t *mpp_data,
                                     const struct mpp_buf *src_buf)
{
    struct ge_bitblt blt = { 0 };
    struct mpp_ge *ge2d_dev = get_ge2d_device();
    enum mpp_pixel_format out_fmt;
    lv_color_format_t out_cf;
    uint8_t *out_raw;
    uintptr_t out_addr;
    int out_stride;
    int out_size;
    int width = src_buf->size.width;
    int height = src_buf->size.height;

    if (!ge2d_dev) {
        LV_LOG_ERROR("GE2D device is NULL");
        return LV_RESULT_INVALID;
    }

#if defined(AICFB_ARGB8888)
    out_fmt = MPP_FMT_ARGB_8888;
    out_stride = ALIGN_16B(width * 4);
#elif defined(AICFB_RGB888)
    out_fmt = MPP_FMT_RGB_888;
    out_stride = ALIGN_16B(width * 3);
#else
    out_fmt = MPP_FMT_RGB_565;
    out_stride = ALIGN_16B(width * 2);
#endif
    out_cf = mpp_fmt_to_lv_fmt(out_fmt);
    out_size = out_stride * height;

    out_raw = (uint8_t *)aicos_malloc_try_cma(out_size + CACHE_LINE_SIZE - 1);
    if (!out_raw) {
        LV_LOG_ERROR("alloc RGB output failed, size:%d", out_size);
        return LV_RESULT_INVALID;
    }

    out_addr = ALIGN_UP((uintptr_t)out_raw, CACHE_LINE_SIZE);
    aicos_dcache_clean_invalid_range((ulong *)out_addr,
                                     ALIGN_UP(out_size, CACHE_LINE_SIZE));

    memcpy(&blt.src_buf, src_buf, sizeof(blt.src_buf));
    blt.dst_buf.buf_type = MPP_PHY_ADDR;
    blt.dst_buf.phy_addr[0] = (unsigned int)out_addr;
    blt.dst_buf.stride[0] = out_stride;
    blt.dst_buf.format = out_fmt;
    blt.dst_buf.size.width = width;
    blt.dst_buf.size.height = height;

    if (mpp_ge_bitblt(ge2d_dev, &blt) < 0 ||
        mpp_ge_emit(ge2d_dev) < 0 ||
        mpp_ge_sync(ge2d_dev) < 0) {
        LV_LOG_ERROR("JPEG YUV to RGB conversion failed");
        aicos_free(MEM_CMA, out_raw);
        return LV_RESULT_INVALID;
    }

    /* RGB 缓冲区生成后替换临时 JPEG YUV 平面。 */
    lv_frame_buf_free(mpp_data);
    mpp_data->data[0] = out_raw;
    mpp_data->decoded.header.cf = out_cf;
    mpp_data->decoded.header.stride = out_stride;
    mpp_data->decoded.data = (uint8_t *)out_addr;
    mpp_data->decoded.data_size = out_size;
    mpp_data->decoded.unaligned_data = out_raw;

    return LV_RESULT_OK;
}

/* 根据文件扩展名或数据头判断需要使用的 MPP 编解码器。 */
static enum mpp_codec_type detect_codec_type(lv_stream_t *stream, lv_image_decoder_dsc_t *dsc)
{
    enum mpp_codec_type type = MPP_CODEC_VIDEO_DECODER_PNG;
    char* ptr = NULL;

    if (lv_image_src_get_type(dsc->src) == LV_IMAGE_SRC_FILE) {
        ptr = strrchr(dsc->src, '.');
        if (ptr && image_suffix_is_jpg(ptr)) {
            type = MPP_CODEC_VIDEO_DECODER_MJPEG;
#ifdef AIC_MPP_AICP_DEC_ENABLE
        } else if (ptr && image_suffix_is_aicp(ptr)) {
            type = MPP_CODEC_VIDEO_DECODER_AICP;
#endif
        }
    } else if (lv_image_src_get_type(dsc->src) == LV_IMAGE_SRC_VARIABLE) {
#ifdef AIC_MPP_AICP_DEC_ENABLE
        uint8_t aicp_header[4];
        uint32_t read_num;
        lv_result_t res;

        // 优先检查 AICP 文件头。
        res = lv_aic_stream_read(stream, aicp_header, 4, &read_num);
        if (res == LV_FS_RES_OK && read_num == 4 && memcmp(aicp_header, "AICP", 4) == 0) {
            type = MPP_CODEC_VIDEO_DECODER_AICP;
        } else {
            lv_aic_stream_reset(stream);
#endif
            if (check_jpeg_soi(stream) == LV_RESULT_OK) {
                type = MPP_CODEC_VIDEO_DECODER_MJPEG;
            } else {
                lv_aic_stream_reset(stream);
            }
#ifdef AIC_MPP_AICP_DEC_ENABLE
        }
#endif
        lv_aic_stream_reset(stream);
    }

    return type;
}

/* 打开 MPP 解码器，完成硬件解码并返回 LVGL 图像数据。 */
static lv_result_t lv_mpp_dec_open(lv_image_decoder_t *decoder, lv_image_decoder_dsc_t *dsc)
{
    lv_result_t res = LV_RESULT_OK;
    uint32_t file_len = 0;
    lv_stream_t stream;
    struct mpp_packet packet;
    int width = 0;
    int height = 0;
    enum mpp_codec_type type;
    struct decode_config config = { 0 };
    int buf_size[3] = { 0 };
    struct mpp_decoder *dec = NULL;
    struct frame_allocator *allocator = NULL;

#if LV_USE_AIC_BMP
    if (lv_image_src_get_type(dsc->src) == LV_IMAGE_SRC_FILE) {
        const char *ext = lv_fs_get_ext(dsc->src);
        if (lv_strcmp(ext, "bmp") == 0 || lv_strcmp(ext, "BMP") == 0) {
            return lv_bmp_dec_open(decoder, dsc);
        }
    } else if (lv_image_src_get_type(dsc->src) == LV_IMAGE_SRC_VARIABLE) {
        if (lv_aic_stream_open(&stream, dsc->src) != LV_FS_RES_OK)
            return LV_RESULT_INVALID;

        if (lv_check_bmp_header(&stream) == LV_RESULT_OK)
            return lv_bmp_dec_open(decoder, dsc);
    }
#endif

    if (lv_aic_stream_open(&stream, dsc->src) != LV_FS_RES_OK)
        return LV_RESULT_INVALID;

    mpp_decoder_data_t *mpp_data = (mpp_decoder_data_t *)lv_malloc_zeroed(sizeof(mpp_decoder_data_t));
    CHECK_PTR(mpp_data);

    width = dsc->header.w;
    height = dsc->header.h;
    config.pix_fmt = lv_fmt_to_mpp_fmt(dsc->header.cf);

    // 判断待解码图像的编码类型。
    type = detect_codec_type(&stream, dsc);

    lv_aic_stream_get_size(&stream, &file_len);
    dec = mpp_decoder_create(type);
    CHECK_PTR(dec);

    config.bitstream_buffer_size = ALIGN_UP(file_len, 256);
    config.extra_frame_num = 0;
    config.packet_count = 1;

    struct mpp_frame dec_frame;
    memset(&dec_frame, 0, sizeof(struct mpp_frame));
    dec_frame.buf.size.width = width;
    dec_frame.buf.size.height = height;
    dec_frame.buf.format = config.pix_fmt;
    dec_frame.buf.buf_type = MPP_PHY_ADDR;

    int size_shift = 0;
#if defined(MPP_JPEG_DEC_OUT_SIZE_LIMIT_ENABLE)
    if (type == MPP_CODEC_VIDEO_DECODER_MJPEG)
        size_shift = dsc->header.reserved_2;
#endif
    lv_set_frame_buf_size(&dec_frame, buf_size, 0);
    if (size_shift > 0) {
        struct mpp_scale_ratio scale;
        scale.hor_scale = size_shift;
        scale.ver_scale = size_shift;
        mpp_decoder_control(dec, MPP_DEC_INIT_CMD_SET_SCALE, &scale);
    }

    CHECK_RET(lv_frame_buf_alloc(mpp_data, &dec_frame.buf, buf_size, dsc->header.cf), LV_RESULT_OK);

    // 分配器由解码器销毁时自动释放。
    allocator = lv_open_allocator(&dec_frame);
    CHECK_PTR(allocator);

    mpp_decoder_control(dec, MPP_DEC_INIT_CMD_SET_EXT_FRAME_ALLOCATOR, (void*)allocator);
    CHECK_RET(mpp_decoder_init(dec, &config), 0);
    memset(&packet, 0, sizeof(struct mpp_packet));
    mpp_decoder_get_packet(dec, &packet, file_len);

    uint32_t read_size = 0;

    lv_aic_stream_read(&stream, packet.data, file_len, &read_size);
    packet.size = file_len;
    packet.flag = PACKET_FLAG_EOS;

    mpp_decoder_put_packet(dec, &packet);
    // if (type == MPP_CODEC_VIDEO_DECODER_MJPEG)
    //     printf("[jpeg] HW decode start, %dx%d, %u bytes, fmt=%d\n",
    //            width, height, file_len, config.pix_fmt);

    int decode_ret = mpp_decoder_decode(dec);
    // if (type == MPP_CODEC_VIDEO_DECODER_MJPEG)
    //     printf("[jpeg] HW decode done, ret=%d\n", decode_ret);
    CHECK_RET(decode_ret, 0);

    struct mpp_frame frame;
    memset(&frame, 0, sizeof(struct mpp_frame));
    mpp_decoder_get_frame(dec, &frame);
    mpp_decoder_put_frame(dec, &frame);

    memcpy(&mpp_data->dec_buf, &frame.buf, sizeof(struct mpp_buf));

    if (type == MPP_CODEC_VIDEO_DECODER_MJPEG &&
        lv_fmt_is_yuv(mpp_data->decoded.header.cf)) {
        /* 先释放解码器及其帧管理器，再用 RGB 缓冲区替换 JPEG YUV 平面。 */
        mpp_decoder_destory(dec);
        dec = NULL;
        // printf("[jpeg] GE2D convert NV12 to RGB start\n");
        CHECK_RET(lv_mpp_yuv_to_rgb(mpp_data, &frame.buf), LV_RESULT_OK);
        // printf("[jpeg] GE2D convert NV12 to RGB done, cf=%d, size=%u\n",
        //        mpp_data->decoded.header.cf, mpp_data->decoded.data_size);
    }

    dsc->decoded = &mpp_data->decoded;

#if LV_CACHE_DEF_SIZE > 0
    if (!dsc->args.no_cache) {
        mpp_cache_t *mpp_cache = (mpp_cache_t *)decoder->user_data;
        if (lv_image_cache_check(mpp_cache)) {
            // 将解码结果加入 LVGL 图片缓存。
            lv_image_cache_data_t search_key;
            search_key.src_type = dsc->src_type;
            search_key.src = dsc->src;
            search_key.slot.size = dsc->decoded->data_size;
            mpp_data->cached = true;
            lv_cache_entry_t *cache_entry = lv_image_decoder_add_to_cache(decoder, &search_key, dsc->decoded, mpp_data);
            CHECK_PTR(cache_entry);
            dsc->cache_entry = cache_entry;
            lv_mutex_lock(&mpp_cache->lock);
            mpp_cache->cache_num++;
            lv_mutex_unlock(&mpp_cache->lock);
        }
    }
#endif

out:
    if (dec)
        mpp_decoder_destory(dec);

    if (res == LV_RESULT_INVALID) {
        dsc->decoded = NULL;
        if (mpp_data) {
            lv_frame_buf_free(mpp_data);
            lv_free(mpp_data);
        }
    }

    lv_aic_stream_close(&stream);
    return res;
}

/* 释放一次解码产生的帧数据和解码上下文。 */
static void mpp_dec_data_release(mpp_decoder_data_t *decoder_data)
{
    if (decoder_data == NULL)
        return;

    if (decoder_data) {
        lv_frame_buf_free(decoder_data);
        lv_free(decoder_data);
    }
}

/* 关闭图片解码描述符并按缓存状态释放数据。 */
static void lv_mpp_dec_close(lv_image_decoder_t *decoder, lv_image_decoder_dsc_t *dsc)
{
    mpp_decoder_data_t *mpp_data =  (mpp_decoder_data_t *)dsc->decoded;
    LV_UNUSED(decoder);

    if (!mpp_data->cached) {
        mpp_dec_data_release(mpp_data);
    } else {
        lv_cache_release(dsc->cache, dsc->cache_entry, NULL);
    }

    return;
}

/* 在 LVGL 淘汰缓存项时释放对应的 MPP 解码资源。 */
static void mpp_dec_cache_free_cb(void *node, void *user_data)
{
    lv_image_cache_data_t *entry = (lv_image_cache_data_t *)node;
    mpp_cache_t *mpp_cache = (mpp_cache_t *)entry->decoder->user_data;

    mpp_dec_data_release((mpp_decoder_data_t *)entry->user_data);
    lv_mutex_lock(&mpp_cache->lock);
    mpp_cache->cache_num--;
    lv_mutex_unlock(&mpp_cache->lock);

    // 释放缓存项复制的文件名。
    if (entry->src_type == LV_IMAGE_SRC_FILE)
        lv_free((void *)entry->src);
}

static lv_image_decoder_t *mpp_dec = NULL;
/* 注册 MPP 图片解码器及其缓存回调。 */
void lv_mpp_dec_init(void)
{
    lv_image_decoder_t *dec = lv_image_decoder_create();
    mpp_cache_t *mpp_cache = (mpp_cache_t *)lv_malloc_zeroed(sizeof(mpp_cache_t));

    if (mpp_cache == NULL)
        return;

    mpp_cache->max_num = LV_CACHE_IMG_NUM;
    lv_mutex_init(&mpp_cache->lock);
    dec->user_data = mpp_cache;
    lv_image_decoder_set_info_cb(dec, lv_mpp_dec_info);
    lv_image_decoder_set_open_cb(dec, lv_mpp_dec_open);
    lv_image_decoder_set_close_cb(dec, lv_mpp_dec_close);

     // 使用自定义缓存释放回调。
    lv_image_decoder_set_cache_free_cb(dec, mpp_dec_cache_free_cb);
    mpp_dec = dec;
}

/* 注销 MPP 图片解码器并释放其私有缓存管理器。 */
void lv_mpp_dec_deinit(void)
{
    lv_image_decoder_t * dec = NULL;
    while ((dec = lv_image_decoder_get_next(dec)) != NULL) {
        if (dec->info_cb == lv_mpp_dec_info) {
            mpp_cache_t *mpp_cache = (mpp_cache_t *)dec->user_data;
            lv_mutex_delete(&mpp_cache->lock);
            lv_free(dec->user_data);
            lv_image_decoder_delete(dec);
            break;
        }
    }
}

#if LV_CACHE_DEF_SIZE > 0
/* 从 LVGL 图片缓存中淘汰一个可释放的缓存项。 */
bool lv_drop_one_cached_image()
{
    #define img_cache_p (LV_GLOBAL_DEFAULT()->img_cache)
    #define lv_initialized  (LV_GLOBAL_DEFAULT()->inited)
    if (lv_initialized)
        return lv_cache_evict_one(img_cache_p, NULL);
    else
        return false;
}

/* 检查缓存数量，必要时淘汰旧图后允许加入新图。 */
bool lv_image_cache_check(mpp_cache_t *mpp_cache)
{
#if LV_CACHE_IMG_NUM_LIMIT == 0
    return true;
#else
    if (mpp_cache->cache_num < mpp_cache->max_num) {
        return true;
    } else {
        if (lv_drop_one_cached_image())
            return true;
        else
            return false;
    }
#endif // 图片缓存数量限制开关。
}
#else
/* 未启用图片缓存时不执行缓存淘汰。 */
bool lv_drop_one_cached_image()
{
    return false;
}
#endif // LV_CACHE_DEF_SIZE

/* 更新 MPP 图片缓存的最大条目数并清空现有缓存。 */
void lv_img_cache_set_size(uint16_t max_num)
{
    lv_image_cache_drop(NULL);
    if (mpp_dec) {
        mpp_cache_t *mpp_cache = (mpp_cache_t *)mpp_dec->user_data;
        mpp_cache->max_num = max_num;
    }
}
