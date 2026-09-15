/*
 * Copyright (C) 2026 ArtInChip Technology Co., Ltd.
 *
 * Minimal AIC player test:
 * tap anywhere to switch between 11.mp4 and 22.mp4.
 */

#include "lv_aic_player.h"
#include "lvgl.h"

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *player;
    lv_obj_t *status_label;
    uint8_t video_index;
} video_switch_demo_t;

static video_switch_demo_t video_demo;

static const char *const video_sources[] = {
    "L:" LVGL_STORAGE_PATH "/video/11.mp4",
    "L:" LVGL_STORAGE_PATH "/video/22.mp4",
};

static const char *const video_names[] = {
    "11.mp4",
    "22.mp4",
};

static void video_demo_play(video_switch_demo_t *demo)
{
    lv_res_t result;

    result = lv_aic_player_set_src(demo->player,
                                   video_sources[demo->video_index]);
    if (result != LV_RES_OK) {
        lv_label_set_text_fmt(demo->status_label,
                              "Open %s failed",
                              video_names[demo->video_index]);
        LV_LOG_ERROR("open video failed: %s",
                     video_sources[demo->video_index]);
        return;
    }

    lv_aic_player_set_auto_restart(demo->player, true);
    lv_aic_player_set_cmd(demo->player, LV_AIC_PLAYER_CMD_START, NULL);
    lv_obj_center(demo->player);

    lv_label_set_text_fmt(demo->status_label,
                          "Playing %s - tap to switch",
                          video_names[demo->video_index]);
    LV_LOG_USER("playing video: %s", video_sources[demo->video_index]);
}

static void video_demo_click_cb(lv_event_t *event)
{
    video_switch_demo_t *demo = lv_event_get_user_data(event);

    if (!demo || !demo->player)
        return;

    /* STOP destroys the current backend. set_src() then creates a new one. */
    lv_aic_player_set_cmd(demo->player, LV_AIC_PLAYER_CMD_STOP, NULL);
    demo->video_index ^= 1U;
    video_demo_play(demo);
}

static void video_switch_demo_init(void)
{
    lv_obj_t *click_area;

    lv_memset_00(&video_demo, sizeof(video_demo));

    video_demo.screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(video_demo.screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(video_demo.screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(video_demo.screen, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(video_demo.screen, 0, LV_PART_MAIN);
    lv_obj_clear_flag(video_demo.screen, LV_OBJ_FLAG_SCROLLABLE);

    video_demo.player = lv_aic_player_create(video_demo.screen);
    /* Explicitly decode into the LVGL UI double buffer, not the video layer. */
    lv_aic_player_set_draw_layer(video_demo.player,
                                 LV_AIC_PLAYER_LAYER_UI_DOUBLE_BUF);

    video_demo.status_label = lv_label_create(video_demo.screen);
    lv_obj_set_style_text_color(video_demo.status_label,
                                lv_color_white(),
                                LV_PART_MAIN);
    lv_obj_align(video_demo.status_label, LV_ALIGN_BOTTOM_MID, 0, -12);

    /* A transparent full-screen object makes the whole display clickable. */
    click_area = lv_obj_create(video_demo.screen);
    lv_obj_set_size(click_area, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(click_area, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(click_area, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(click_area, 0, LV_PART_MAIN);
    lv_obj_add_flag(click_area, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(click_area, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(click_area,
                        video_demo_click_cb,
                        LV_EVENT_CLICKED,
                        &video_demo);

    lv_scr_load(video_demo.screen);
    video_demo_play(&video_demo);
}

void ui_init(void)
{
    video_switch_demo_init();
}
