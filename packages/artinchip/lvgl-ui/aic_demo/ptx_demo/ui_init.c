/* Black-screen LVGL demo. Wi-Fi scanning is provided by the `wifi scan` shell command. */

#include <rtthread.h>
#include <wlan_mgnt.h>

#include "lvgl.h"

#define WIFI_STA_START_DELAY_MS 3000U

static void wifi_sta_start_cb(lv_timer_t *timer)
{
    int ret;

    ret = rt_wlan_set_mode(RT_WLAN_DEVICE_STA_NAME, RT_WLAN_STATION);
    if (ret != RT_EOK)
        rt_kprintf("[wifi] cannot set %s to STA mode: %d\n",
                   RT_WLAN_DEVICE_STA_NAME, ret);

    lv_timer_delete(timer);
}

void ui_init(void)
{
    lv_obj_t *screen = lv_obj_create(NULL);

    if (!screen)
        return;

    lv_obj_set_size(screen, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(screen, 0, LV_PART_MAIN);
    lv_scr_load(screen);

    lv_timer_create(wifi_sta_start_cb, WIFI_STA_START_DELAY_MS, NULL);
}
