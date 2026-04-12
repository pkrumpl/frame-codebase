/*
 * Experiment Common Implementation
 *
 * Shared utilities for JPEG decoding and image processing used by all experiments.
 */

#include <string.h>
#include "experiment_common.h"
#include "error_logging.h"
#include "tjpgd.h"

/*-----------------------------------------------*/
/* Test JPEG Data (DEV_KIT builds only)          */
/*-----------------------------------------------*/

#include "test_jpeg_data.h"

/*-----------------------------------------------*/
/* JPEG Decoder Callbacks                        */
/*-----------------------------------------------*/

/* Input function for TJpgDec - reads from memory buffer */
static size_t jpeg_input_func(JDEC *jd, uint8_t *buff, size_t ndata)
{
    jpeg_ctx_t *ctx = (jpeg_ctx_t *)jd->device;

    if (ctx->offset >= ctx->size) {
        return 0;  /* No more data */
    }

    size_t remaining = ctx->size - ctx->offset;
    size_t to_read = (ndata < remaining) ? ndata : remaining;

    if (buff) {
        memcpy(buff, ctx->data + ctx->offset, to_read);
    }
    ctx->offset += to_read;

    return to_read;
}

/* Output function for TJpgDec - stores grayscale pixels with 90 CCW rotation */
static int jpeg_output_func(JDEC *jd, void *bitmap, JRECT *rect)
{
    jpeg_ctx_t *ctx = (jpeg_ctx_t *)jd->device;
    uint8_t *src = (uint8_t *)bitmap;

    /* Copy each row of the rectangle to the output buffer with 90 CCW rotation.
     * This corrects for the camera sensor orientation.
     * Original (x, y) maps to (height - 1 - y, x) in rotated output.
     * For square images: dst_idx = x * width + (width - 1 - y)
     */
    for (uint16_t y = rect->top; y <= rect->bottom; y++) {
        if (y >= ctx->height) break;

        for (uint16_t x = rect->left; x <= rect->right; x++) {
            if (x >= ctx->width) continue;

            size_t src_idx = (y - rect->top) * (rect->right - rect->left + 1) + (x - rect->left);
            /* 90 CCW rotation: new_x = y, new_y = width - 1 - x */
            size_t dst_idx = x * ctx->buf_width + (ctx->width - 1 - y);
            ctx->buffer[dst_idx] = src[src_idx];
        }
    }

    return 1;  /* Continue decoding */
}

/* Output function for TJpgDec - stores grayscale pixels WITHOUT rotation */
static int jpeg_output_func_no_rotation(JDEC *jd, void *bitmap, JRECT *rect)
{
    jpeg_ctx_t *ctx = (jpeg_ctx_t *)jd->device;
    uint8_t *src = (uint8_t *)bitmap;

    /* Copy each row of the rectangle to the output buffer without any rotation.
     * Used for intermediate buffer before upscale+rotate step.
     */
    for (uint16_t y = rect->top; y <= rect->bottom; y++) {
        if (y >= ctx->height) break;

        for (uint16_t x = rect->left; x <= rect->right; x++) {
            if (x >= ctx->width) continue;

            size_t src_idx = (y - rect->top) * (rect->right - rect->left + 1) + (x - rect->left);
            size_t dst_idx = y * ctx->buf_width + x;
            ctx->buffer[dst_idx] = src[src_idx];
        }
    }

    return 1;  /* Continue decoding */
}

/*-----------------------------------------------*/
/* JPEG Decode Functions                         */
/*-----------------------------------------------*/

int jpeg_decode_grayscale_scaled(const uint8_t *jpeg_data, size_t jpeg_size,
                                  uint8_t *out_buffer, uint16_t out_width, uint16_t out_height,
                                  uint16_t *actual_width, uint16_t *actual_height,
                                  uint8_t scale, bool apply_rotation)
{
    JDEC jdec;
    jpeg_ctx_t ctx;
    JRESULT res;

    /* Work area for TJpgDec - needs about 3KB for baseline JPEG */
    static uint8_t work_pool[4096];

    /* Setup context - used for both input and output */
    ctx.data = jpeg_data;
    ctx.size = jpeg_size;
    ctx.offset = 0;
    ctx.buffer = out_buffer;
    ctx.buf_width = out_width;

    /* Prepare decoder */
    res = jd_prepare(&jdec, jpeg_input_func, work_pool, sizeof(work_pool), &ctx);
    if (res != JDR_OK) {
        LOG("JPEG prepare failed: %d", res);
        return -res;
    }

    LOG("JPEG: %dx%d (scale=%d)", jdec.width, jdec.height, scale);

    /* Calculate scaled dimensions */
    uint16_t scaled_width = jdec.width >> scale;
    uint16_t scaled_height = jdec.height >> scale;

    /* Return actual dimensions (after scaling) */
    if (actual_width) *actual_width = scaled_width;
    if (actual_height) *actual_height = scaled_height;

    /* Setup output dimensions (clamp to buffer size) */
    ctx.width = (out_width < scaled_width) ? out_width : scaled_width;
    ctx.height = (out_height < scaled_height) ? out_height : scaled_height;

    /* Decompress with specified scale factor */
    res = jd_decomp(&jdec, apply_rotation ? jpeg_output_func : jpeg_output_func_no_rotation, scale);
    if (res != JDR_OK) {
        LOG("JPEG decompress failed: %d", res);
        return -res;
    }

    return 0;
}

int jpeg_decode_grayscale(const uint8_t *jpeg_data, size_t jpeg_size,
                          uint8_t *out_buffer, uint16_t out_width, uint16_t out_height,
                          uint16_t *actual_width, uint16_t *actual_height)
{
    return jpeg_decode_grayscale_scaled(jpeg_data, jpeg_size, out_buffer,
                                         out_width, out_height,
                                         actual_width, actual_height,
                                         0, true);  /* scale=0 (1:1), rotation=true */
}

/*-----------------------------------------------*/
/* Image Processing Functions                    */
/*-----------------------------------------------*/

void upscale_90_to_96_with_rotation(const uint8_t *src, uint8_t *dst)
{
    const int SRC_SIZE = 90;
    const int DST_SIZE = 96;
    const float scale = (float)(SRC_SIZE - 1) / (float)(DST_SIZE - 1);  /* 89/95 */

    for (int dy = 0; dy < DST_SIZE; dy++) {
        for (int dx = 0; dx < DST_SIZE; dx++) {
            float sx = dx * scale;
            float sy = dy * scale;
            int x0 = (int)sx;
            int y0 = (int)sy;
            int x1 = (x0 < SRC_SIZE - 1) ? x0 + 1 : SRC_SIZE - 1;
            int y1 = (y0 < SRC_SIZE - 1) ? y0 + 1 : SRC_SIZE - 1;
            float fx = sx - x0;
            float fy = sy - y0;

            /* Bilinear interpolation */
            float v = src[y0 * SRC_SIZE + x0] * (1 - fx) * (1 - fy) +
                      src[y0 * SRC_SIZE + x1] * fx * (1 - fy) +
                      src[y1 * SRC_SIZE + x0] * (1 - fx) * fy +
                      src[y1 * SRC_SIZE + x1] * fx * fy;

            /* 90 CW rotation during write: (dx, dy) -> (DST_SIZE-1-dx, dy) */
            dst[(DST_SIZE - 1 - dx) * DST_SIZE + dy] = (uint8_t)(v + 0.5f);
        }
    }
}

#if defined(ML_EXPERIMENT_VWW_RGB)
/*-----------------------------------------------*/
/* RGB JPEG Decoder Callbacks                    */
/*-----------------------------------------------*/

/* Output function for TJpgDec - stores RGB888 pixels with 90 CCW rotation */
static int jpeg_output_func_rgb(JDEC *jd, void *bitmap, JRECT *rect)
{
    jpeg_ctx_t *ctx = (jpeg_ctx_t *)jd->device;
    uint8_t *src = (uint8_t *)bitmap;

    uint16_t src_width = rect->right - rect->left + 1;

    /* Copy each row of the rectangle to the output buffer with 90 CCW rotation.
     * Original (x, y) maps to (y, width-1-x) in rotated output.
     */
    for (uint16_t y = rect->top; y <= rect->bottom; y++) {
        if (y >= ctx->height) break;

        for (uint16_t x = rect->left; x <= rect->right; x++) {
            if (x >= ctx->width) continue;

            /* 90 CCW rotation: new_x = y, new_y = width - 1 - x */
            uint16_t rot_x = y;
            uint16_t rot_y = ctx->width - 1 - x;

            size_t dst_idx = (rot_y * ctx->buf_width + rot_x) * 3;
            size_t src_idx = ((y - rect->top) * src_width + (x - rect->left)) * 3;

            ctx->buffer[dst_idx + 0] = src[src_idx + 0];  /* R */
            ctx->buffer[dst_idx + 1] = src[src_idx + 1];  /* G */
            ctx->buffer[dst_idx + 2] = src[src_idx + 2];  /* B */
        }
    }

    return 1;  /* Continue decoding */
}

/* Output function for TJpgDec - stores RGB888 pixels WITHOUT rotation */
static int jpeg_output_func_rgb_no_rotation(JDEC *jd, void *bitmap, JRECT *rect)
{
    jpeg_ctx_t *ctx = (jpeg_ctx_t *)jd->device;
    uint8_t *src = (uint8_t *)bitmap;

    uint16_t src_width = rect->right - rect->left + 1;

    /* Copy each row of the rectangle to the output buffer without any rotation. */
    for (uint16_t y = rect->top; y <= rect->bottom; y++) {
        if (y >= ctx->height) break;

        for (uint16_t x = rect->left; x <= rect->right; x++) {
            if (x >= ctx->width) continue;

            size_t dst_idx = (y * ctx->buf_width + x) * 3;
            size_t src_idx = ((y - rect->top) * src_width + (x - rect->left)) * 3;

            ctx->buffer[dst_idx + 0] = src[src_idx + 0];  /* R */
            ctx->buffer[dst_idx + 1] = src[src_idx + 1];  /* G */
            ctx->buffer[dst_idx + 2] = src[src_idx + 2];  /* B */
        }
    }

    return 1;  /* Continue decoding */
}

/*-----------------------------------------------*/
/* RGB JPEG Decode Functions                     */
/*-----------------------------------------------*/

int jpeg_decode_rgb_scaled(const uint8_t *jpeg_data, size_t jpeg_size,
                           uint8_t *out_buffer, uint16_t out_width, uint16_t out_height,
                           uint16_t *actual_width, uint16_t *actual_height,
                           uint8_t scale, bool apply_rotation)
{
    JDEC jdec;
    jpeg_ctx_t ctx;
    JRESULT res;

    /* Work area for TJpgDec - needs about 3KB for baseline JPEG */
    static uint8_t work_pool[4096];

    /* Setup context - used for both input and output */
    ctx.data = jpeg_data;
    ctx.size = jpeg_size;
    ctx.offset = 0;
    ctx.buffer = out_buffer;
    ctx.buf_width = out_width;

    /* Prepare decoder */
    res = jd_prepare(&jdec, jpeg_input_func, work_pool, sizeof(work_pool), &ctx);
    if (res != JDR_OK) {
        LOG("JPEG RGB prepare failed: %d", res);
        return -res;
    }

    LOG("JPEG RGB: %dx%d (scale=%d)", jdec.width, jdec.height, scale);

    /* Calculate scaled dimensions */
    uint16_t scaled_width = jdec.width >> scale;
    uint16_t scaled_height = jdec.height >> scale;

    /* Return actual dimensions (after scaling) */
    if (actual_width) *actual_width = scaled_width;
    if (actual_height) *actual_height = scaled_height;

    /* Setup output dimensions (clamp to buffer size) */
    ctx.width = (out_width < scaled_width) ? out_width : scaled_width;
    ctx.height = (out_height < scaled_height) ? out_height : scaled_height;

    /* Decompress with specified scale factor */
    res = jd_decomp(&jdec, apply_rotation ? jpeg_output_func_rgb : jpeg_output_func_rgb_no_rotation, scale);
    if (res != JDR_OK) {
        LOG("JPEG RGB decompress failed: %d", res);
        return -res;
    }

    return 0;
}

/*-----------------------------------------------*/
/* RGB Image Processing Functions                */
/*-----------------------------------------------*/

void upscale_90_to_96_rgb_with_rotation(const uint8_t *src, uint8_t *dst)
{
    const int SRC_SIZE = 90;
    const int DST_SIZE = 96;
    const float scale = (float)(SRC_SIZE - 1) / (float)(DST_SIZE - 1);  /* 89/95 */

    for (int dy = 0; dy < DST_SIZE; dy++) {
        for (int dx = 0; dx < DST_SIZE; dx++) {
            float sx = dx * scale;
            float sy = dy * scale;
            int x0 = (int)sx;
            int y0 = (int)sy;
            int x1 = (x0 < SRC_SIZE - 1) ? x0 + 1 : SRC_SIZE - 1;
            int y1 = (y0 < SRC_SIZE - 1) ? y0 + 1 : SRC_SIZE - 1;
            float fx = sx - x0;
            float fy = sy - y0;

            /* Process each color channel */
            for (int c = 0; c < 3; c++) {
                float v00 = src[(y0 * SRC_SIZE + x0) * 3 + c];
                float v10 = src[(y0 * SRC_SIZE + x1) * 3 + c];
                float v01 = src[(y1 * SRC_SIZE + x0) * 3 + c];
                float v11 = src[(y1 * SRC_SIZE + x1) * 3 + c];

                /* Bilinear interpolation */
                float v = v00 * (1 - fx) * (1 - fy) +
                          v10 * fx * (1 - fy) +
                          v01 * (1 - fx) * fy +
                          v11 * fx * fy;

                /* 90 CCW rotation during write: (dx, dy) -> (dy, DST_SIZE-1-dx) */
                dst[((DST_SIZE - 1 - dx) * DST_SIZE + dy) * 3 + c] = (uint8_t)(v + 0.5f);
            }
        }
    }
}

#endif /* ML_EXPERIMENT_VWW_RGB */

/*-----------------------------------------------*/
/* Experiment Library Registration               */
/*-----------------------------------------------*/

void lua_open_experiment_library(lua_State *L)
{
    lua_getglobal(L, "frame");

    /* New table to have nested commands for experiments */
    lua_newtable(L);

    /* Let the experiment-specific module register its functions */
    experiment_register_lua_functions(L, lua_gettop(L));

    lua_setfield(L, -2, "experiment");

    lua_pop(L, 1);
}
