/*
 * main.c
 * 基于 LVGL 的网络可控贪吃蛇 —— 程序入口
 *
 * 硬件：ARM Linux 开发板（aarch64）
 *   显示：帧缓冲设备 /dev/fb0
 *   输入：触摸屏     /dev/input/event6
 *
 * 主线程职责：
 *   - 周期性调用 lv_timer_handler() 驱动 LVGL；
 *   - 在该安全上下文中消费网络线程置位的暂停 / 重置请求。
 */

#include <unistd.h>
#include <stdio.h>

#include "snake_server.h"

int main(void)
{
    lv_init();

    /* 初始化帧缓冲设备 -> 显示屏 */
    lv_display_t *disp = lv_linux_fbdev_create();
    lv_linux_fbdev_set_file(disp, "/dev/fb0");

    /* 初始化输入设备 -> 触摸屏 */
    lv_evdev_create(LV_INDEV_TYPE_POINTER, "/dev/input/event6");

    /* 创建游戏界面并启动网络控制 */
    init_game_style();
    init_game_ui();
    start_network_control();

    while (1) {
        lv_timer_handler();

        /* 处理网络暂停切换请求（在主线程执行 LVGL 操作） */
        if (net_pause_toggle) {
            pthread_mutex_lock(&game_mutex);
            net_pause_toggle = false;
            if (!game_over) {
                game_paused = !game_paused;
                /* 切换图片和定时器 */
                if (game_paused) {
                    if (move_timer) lv_timer_pause(move_timer);
                } else {
                    if (move_timer) lv_timer_resume(move_timer);
                }
                switch_pause_img();
            }
            pthread_mutex_unlock(&game_mutex);
        }

        /* 处理网络重置请求 */
        if (net_reset_request) {
            net_reset_request = false;
            reset_game();   /* reset_game 内部已加锁，且操作 UI 安全 */
            printf("[网络] 执行重来\n");
        }

        usleep(5000);
    }

    return 0;
}
