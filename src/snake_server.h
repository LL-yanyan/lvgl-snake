#ifndef SNAKE_SERVER_H
#define SNAKE_SERVER_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl/lvgl.h"
#include <pthread.h>
#include <stdbool.h>

/* 蛇的移动方向 */
typedef enum {
    DIR_UP,
    DIR_DOWN,
    DIR_LEFT,
    DIR_RIGHT,
    DIR_NONE
} snake_dir_t;

/*
 * 以下共享状态定义在 snake_server.c，
 * 供 main 主循环在“安全上下文”中消费网络线程产生的请求。
 */
extern pthread_mutex_t game_mutex;   /* 保护所有游戏共享状态 */
extern lv_timer_t     *move_timer;   /* 蛇身步进定时器 */
extern bool            game_over;    /* 游戏是否结束 */
extern bool            game_paused;  /* 游戏是否暂停 */

/* 网络线程 -> 主线程 的异步通知标志（网络线程只置位，不碰 LVGL） */
extern volatile bool net_pause_toggle;  /* 请求切换暂停/继续 */
extern volatile bool net_reset_request; /* 请求重置游戏 */

/* 初始化样式与游戏界面（在 LVGL 初始化之后调用） */
void init_game_style(void);
void init_game_ui(void);

/* 启动网络控制线程（内部创建并 detach） */
void start_network_control(void);

/* 重置游戏；内部已加锁，且只在主线程调用以保证 UI 安全 */
void reset_game(void);

/* 根据 game_paused 切换暂停按钮上的图片 */
void switch_pause_img(void);

#ifdef __cplusplus
}
#endif

#endif /* SNAKE_SERVER_H */
