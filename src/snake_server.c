/*
 * snake_server.c
 * 基于 LVGL 的网络可控贪吃蛇 —— 游戏逻辑 + TCP 网络控制服务端
 *
 * 设计要点：
 *   1. LVGL 非线程安全，所有 LVGL API 只允许在主线程调用；
 *   2. 网络线程（network_thread_func）只做 socket IO，通过互斥锁修改
 *      next_dir，或仅置位 net_pause_toggle / net_reset_request；
 *   3. 暂停 / 重置等需要操作 UI 的动作，由 main 主循环在 lv_timer_handler
 *      上下文中检测标志位后统一执行。
 */

#include "snake_server.h"

#include <unistd.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

/*************************
 * 宏定义
 *************************/
#define GRID_SIZE            20   /* 每个逻辑网格的像素边长 */
#define MOVE_INTERVAL        300  /* 蛇身步进周期，单位 ms */
#define MAX_SNAKE_LEN        200
#define BTN_SIZE             50
#define BTN_MARGIN           10
#define BORDER_WIDTH         5
#define GAME_AREA_MARGIN     10
#define SCORE_OFFSET_X       -20
#define SCORE_OFFSET_Y       20
#define IMG_PATH_PREFIX      "./"
#define PAUSE_RESET_BTN_SIZE 60
#define PAUSE_RESET_MARGIN   15
#define BTN_SPACING          10
#define PAUSE_RESET_IMG_SIZE (PAUSE_RESET_BTN_SIZE - 10)

/* 网络相关配置（请根据开发板实际 IP 修改） */
#define NET_PORT 10086
#define NET_IP   "192.168.118.148"   /* 开发板 IP，需已绑定到网卡 */

/*************************
 * 全局变量
 *************************/
/* 视觉区域和逻辑区域 */
static lv_coord_t game_area_x, game_area_y, game_area_w, game_area_h;

/* UI 控件与游戏状态 */
static lv_obj_t  *game_container;
static lv_obj_t  *snake_body[MAX_SNAKE_LEN];
static lv_obj_t  *food_obj;
static lv_obj_t  *score_label;
static lv_point_t snake_pos[MAX_SNAKE_LEN];
static lv_point_t food_pos;
static uint16_t   snake_len = 3;
static snake_dir_t curr_dir = DIR_RIGHT, next_dir = DIR_RIGHT;
static uint32_t   score = 0;
bool              game_over = false;
lv_timer_t       *move_timer = NULL;
static lv_obj_t  *pause_btn, *reset_btn, *pause_img, *reset_img;
bool              game_paused = false;

/* 网络控制相关 */
static int       net_sockfd = -1;
static pthread_t net_thread;
pthread_mutex_t  game_mutex = PTHREAD_MUTEX_INITIALIZER;   /* 保护所有共享状态 */
volatile bool    net_pause_toggle = false;                 /* 网络请求暂停切换标志 */
volatile bool    net_reset_request = false;                /* 网络请求重置标志 */

/* 样式 */
static lv_style_t style_snake, style_food, style_container, style_score, style_btn, style_btn_img;

/*************************
 * 内部函数声明
 *************************/
static void generate_food(void);
static void draw_snake(void);
static void move_snake(lv_timer_t *timer);
static bool check_collision(lv_point_t *new_head);
static void game_over_handle(void);
static void direction_event_cb(lv_event_t *e);
static void button_dir_cb(lv_event_t *e);
static lv_point_t grid_to_pixel(lv_point_t grid_pos);
static lv_point_t pixel_to_grid(lv_point_t pixel_pos);
static void pause_reset_btn_cb(lv_event_t *e);
static void *network_thread_func(void *arg);

/*************************
 * 坐标转换
 *************************/
static lv_point_t grid_to_pixel(lv_point_t grid_pos) {
    lv_point_t pixel_pos;
    pixel_pos.x = game_area_x + BORDER_WIDTH + grid_pos.x * GRID_SIZE;
    pixel_pos.y = game_area_y + BORDER_WIDTH + grid_pos.y * GRID_SIZE;
    return pixel_pos;
}

static lv_point_t pixel_to_grid(lv_point_t pixel_pos) {
    lv_point_t grid_pos;
    grid_pos.x = (pixel_pos.x - game_area_x - BORDER_WIDTH) / GRID_SIZE;
    grid_pos.y = (pixel_pos.y - game_area_y - BORDER_WIDTH) / GRID_SIZE;
    grid_pos.x = LV_CLAMP(0, grid_pos.x, (game_area_w / GRID_SIZE) - 1);
    grid_pos.y = LV_CLAMP(0, grid_pos.y, (game_area_h / GRID_SIZE) - 1);
    return grid_pos;
}

/*************************
 * 样式初始化
 *************************/
void init_game_style(void) {
    lv_style_init(&style_snake);
    lv_style_set_bg_color(&style_snake, lv_color_hex(0x00FF00));
    lv_style_set_bg_opa(&style_snake, LV_OPA_COVER);
    lv_style_set_radius(&style_snake, 4);

    lv_style_init(&style_food);
    lv_style_set_bg_color(&style_food, lv_color_hex(0xFF0000));
    lv_style_set_bg_opa(&style_food, LV_OPA_COVER);
    lv_style_set_radius(&style_food, GRID_SIZE / 2);

    lv_style_init(&style_container);
    lv_style_set_bg_color(&style_container, lv_color_hex(0x1E1E1E));
    lv_style_set_bg_opa(&style_container, LV_OPA_COVER);
    lv_style_set_border_width(&style_container, BORDER_WIDTH);
    lv_style_set_border_color(&style_container, lv_color_hex(0xFFFFFF));
    lv_style_set_radius(&style_container, 4);

    lv_style_init(&style_score);
    lv_style_set_text_color(&style_score, lv_color_hex(0xFFFFFF));
    lv_style_set_text_font(&style_score, &lv_font_montserrat_20);
    lv_style_set_bg_color(&style_score, lv_color_hex(0x222222));
    lv_style_set_bg_opa(&style_score, LV_OPA_80);
    lv_style_set_pad_left(&style_score, 15);
    lv_style_set_pad_right(&style_score, 15);
    lv_style_set_pad_top(&style_score, 8);
    lv_style_set_pad_bottom(&style_score, 8);
    lv_style_set_radius(&style_score, 12);
    lv_style_set_border_width(&style_score, 1);
    lv_style_set_border_color(&style_score, lv_color_hex(0x666666));

    lv_style_init(&style_btn);
    lv_style_set_size(&style_btn, BTN_SIZE, BTN_SIZE);
    lv_style_set_bg_color(&style_btn, lv_color_hex(0x4A4A4A));
    lv_style_set_bg_opa(&style_btn, LV_OPA_COVER);
    lv_style_set_border_width(&style_btn, 2);
    lv_style_set_border_color(&style_btn, lv_color_hex(0x00FF00));
    lv_style_set_radius(&style_btn, 8);
    lv_style_set_pad_all(&style_btn, 0);

    lv_style_init(&style_btn_img);
    lv_style_set_align(&style_btn_img, LV_ALIGN_CENTER);
    lv_style_set_bg_opa(&style_btn_img, LV_OPA_TRANSP);
}

/*************************
 * 生成食物
 *************************/
static void generate_food(void) {
    if (food_obj) {
        lv_obj_del(food_obj);
        food_obj = NULL;
    }
    bool overlap;
    uint16_t max_col = (game_area_w - 2 * BORDER_WIDTH) / GRID_SIZE;
    uint16_t max_row = (game_area_h - 2 * BORDER_WIDTH) / GRID_SIZE;
    do {
        overlap = false;
        food_pos.x = rand() % (max_col - 1);
        food_pos.y = rand() % (max_row - 1);
        for (uint16_t i = 0; i < snake_len; i++) {
            if (snake_pos[i].x == food_pos.x && snake_pos[i].y == food_pos.y) {
                overlap = true;
                break;
            }
        }
    } while (overlap);
    lv_point_t food_pixel = grid_to_pixel(food_pos);
    food_obj = lv_obj_create(game_container);
    lv_obj_add_style(food_obj, &style_food, LV_STATE_DEFAULT);
    lv_obj_set_size(food_obj, GRID_SIZE, GRID_SIZE);
    lv_obj_set_pos(food_obj, food_pixel.x - game_area_x, food_pixel.y - game_area_y);
}

/*************************
 * 绘制蛇
 *************************/
static void draw_snake(void) {
    for (uint16_t i = 0; i < MAX_SNAKE_LEN; i++) {
        if (snake_body[i]) {
            lv_obj_del(snake_body[i]);
            snake_body[i] = NULL;
        }
    }
    for (uint16_t i = 0; i < snake_len; i++) {
        lv_point_t snake_pixel = grid_to_pixel(snake_pos[i]);
        snake_body[i] = lv_obj_create(game_container);
        lv_obj_add_style(snake_body[i], &style_snake, LV_STATE_DEFAULT);
        lv_obj_set_size(snake_body[i], GRID_SIZE, GRID_SIZE);
        lv_obj_set_pos(snake_body[i], snake_pixel.x - game_area_x, snake_pixel.y - game_area_y);
    }
}

/*************************
 * 碰撞检测
 *************************/
static bool check_collision(lv_point_t *new_head) {
    uint16_t max_col = (game_area_w - 2 * BORDER_WIDTH) / GRID_SIZE - 1;
    uint16_t max_row = (game_area_h - 2 * BORDER_WIDTH) / GRID_SIZE - 1;
    if (new_head->x < 0 || new_head->x > max_col || new_head->y < 0 || new_head->y > max_row)
        return true;
    for (uint16_t i = 1; i < snake_len; i++) {
        if (new_head->x == snake_pos[i].x && new_head->y == snake_pos[i].y)
            return true;
    }
    return false;
}

/*************************
 * 游戏结束处理
 *************************/
static void game_over_handle(void) {
    pthread_mutex_lock(&game_mutex);
    game_over = true;
    if (move_timer != NULL) lv_timer_pause(move_timer);
    game_paused = false;
    pthread_mutex_unlock(&game_mutex);

    switch_pause_img();   /* 切换图片（在主线程，安全） */
    lv_label_set_text_fmt(score_label, "Game Over! Score: %d (Click to Reset)", score);
    lv_obj_t *msg_box = lv_obj_create(lv_screen_active());
    lv_obj_set_size(msg_box, 300, 150);
    lv_obj_set_x(msg_box, game_area_x + (game_area_w - 300) / 2);
    lv_obj_set_y(msg_box, game_area_y + (game_area_h - 150) / 2);
    lv_obj_add_style(msg_box, &style_container, LV_STATE_DEFAULT);
    lv_obj_t *msg_label = lv_label_create(msg_box);
    lv_label_set_text_fmt(msg_label, "Game Over!\nScore: %d\nClick to Reset", score);
    lv_obj_set_align(msg_label, LV_ALIGN_CENTER);
    lv_obj_add_style(msg_label, &style_score, LV_STATE_DEFAULT);
    lv_obj_add_event_cb(msg_box, direction_event_cb, LV_EVENT_CLICKED, NULL);
}

/*************************
 * 蛇移动逻辑
 *************************/
static void move_snake(lv_timer_t *timer) {
    (void)timer;
    if (game_over || game_paused) return;

    pthread_mutex_lock(&game_mutex);
    curr_dir = next_dir;
    pthread_mutex_unlock(&game_mutex);

    lv_point_t new_head = snake_pos[0];
    switch (curr_dir) {
        case DIR_UP:    new_head.y--; break;
        case DIR_DOWN:  new_head.y++; break;
        case DIR_LEFT:  new_head.x--; break;
        case DIR_RIGHT: new_head.x++; break;
        default: return;
    }
    if (check_collision(&new_head)) {
        game_over_handle();
        return;
    }
    bool eat_food = (new_head.x == food_pos.x && new_head.y == food_pos.y);
    for (uint16_t i = snake_len - 1; i > 0; i--)
        snake_pos[i] = snake_pos[i - 1];
    if (eat_food) {
        snake_len++;
        snake_len = LV_MIN(snake_len, (game_area_w / GRID_SIZE) * (game_area_h / GRID_SIZE));
        score += 10;
        lv_label_set_text_fmt(score_label, "Score: %d", score);
        generate_food();
    }
    snake_pos[0] = new_head;
    draw_snake();
}

/*************************
 * 方向控制回调（点击游戏区域，按蛇头相对位置转向）
 *************************/
static void direction_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        if (game_paused) return;
        if (game_over) {
            reset_game();
            return;
        }
        lv_point_t screen_pos;
        lv_indev_t *indev = lv_event_get_indev(e);
        if (indev == NULL) return;
        lv_indev_get_point(indev, &screen_pos);
        lv_point_t click_grid = pixel_to_grid(screen_pos);
        lv_point_t head_grid = snake_pos[0];
        int dx = click_grid.x - head_grid.x;
        int dy = click_grid.y - head_grid.y;

        pthread_mutex_lock(&game_mutex);
        if (abs(dx) > abs(dy)) {
            if (dx > 0 && curr_dir != DIR_LEFT) next_dir = DIR_RIGHT;
            else if (dx < 0 && curr_dir != DIR_RIGHT) next_dir = DIR_LEFT;
        } else {
            if (dy > 0 && curr_dir != DIR_UP) next_dir = DIR_DOWN;
            else if (dy < 0 && curr_dir != DIR_DOWN) next_dir = DIR_UP;
        }
        pthread_mutex_unlock(&game_mutex);
    }
}

/*************************
 * 方向按钮回调
 *************************/
static void button_dir_cb(lv_event_t *e) {
    if (game_over || game_paused) return;
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        snake_dir_t dir = (snake_dir_t)lv_event_get_user_data(e);
        pthread_mutex_lock(&game_mutex);
        switch (dir) {
            case DIR_UP:    if (curr_dir != DIR_DOWN) next_dir = dir; break;
            case DIR_DOWN:  if (curr_dir != DIR_UP)   next_dir = dir; break;
            case DIR_LEFT:  if (curr_dir != DIR_RIGHT) next_dir = dir; break;
            case DIR_RIGHT: if (curr_dir != DIR_LEFT)  next_dir = dir; break;
            default: break;
        }
        pthread_mutex_unlock(&game_mutex);
    }
}

/*************************
 * 切换暂停按钮图片
 *************************/
void switch_pause_img(void) {
    if (game_paused)
        lv_image_set_src(pause_img, IMG_PATH_PREFIX "continue.bmp");
    else
        lv_image_set_src(pause_img, IMG_PATH_PREFIX "pause.bmp");
}

/*************************
 * 暂停 / 重来按钮回调
 *************************/
static void pause_reset_btn_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int btn_type = (int)lv_event_get_user_data(e);
    if (btn_type == 0) { /* 暂停/继续 */
        pthread_mutex_lock(&game_mutex);
        if (game_over) {
            pthread_mutex_unlock(&game_mutex);
            return;
        }
        game_paused = !game_paused;
        pthread_mutex_unlock(&game_mutex);

        /* 切换图片和定时器（必须在主线程） */
        switch_pause_img();
        if (game_paused) {
            if (move_timer) lv_timer_pause(move_timer);
        } else {
            if (move_timer) lv_timer_resume(move_timer);
        }
    } else if (btn_type == 1) { /* 重来 */
        reset_game();
    }
}

/*************************
 * 重置游戏
 *************************/
void reset_game(void) {
    pthread_mutex_lock(&game_mutex);
    snake_len = 3;
    curr_dir = DIR_RIGHT;
    next_dir = DIR_RIGHT;
    score = 0;
    game_over = false;
    game_paused = false;
    pthread_mutex_unlock(&game_mutex);

    /* 更新 UI（必须在主线程） */
    uint16_t mid_col = ((game_area_w - 2 * BORDER_WIDTH) / GRID_SIZE) / 2;
    uint16_t mid_row = ((game_area_h - 2 * BORDER_WIDTH) / GRID_SIZE) / 2;
    snake_pos[0].x = mid_col;
    snake_pos[0].y = mid_row;
    snake_pos[1].x = mid_col - 1;
    snake_pos[1].y = mid_row;
    snake_pos[2].x = mid_col - 2;
    snake_pos[2].y = mid_row;
    lv_label_set_text_fmt(score_label, "Score: %d", score);
    generate_food();
    draw_snake();

    switch_pause_img();   /* 恢复为暂停图片 */
    if (move_timer) lv_timer_resume(move_timer);

    /* 删除游戏结束弹窗 */
    lv_obj_t *msg_box = lv_obj_get_child(lv_screen_active(), -1);
    if (msg_box && lv_obj_get_height(msg_box) == 150)
        lv_obj_del(msg_box);
}

/*************************
 * 初始化游戏 UI
 *************************/
void init_game_ui(void) {
    srand(time(NULL));
    lv_obj_t *screen = lv_screen_active();
    lv_coord_t screen_w = lv_obj_get_width(screen);
    lv_coord_t screen_h = lv_obj_get_height(screen);

    game_area_x = GAME_AREA_MARGIN;
    game_area_y = GAME_AREA_MARGIN;
    game_area_w = screen_w - 2 * GAME_AREA_MARGIN - BTN_SIZE * 3 - BTN_MARGIN * 4;
    game_area_h = screen_h - 2 * GAME_AREA_MARGIN;
    game_area_w = (game_area_w / GRID_SIZE) * GRID_SIZE;
    game_area_h = (game_area_h / GRID_SIZE) * GRID_SIZE;

    game_container = lv_obj_create(screen);
    lv_obj_add_style(game_container, &style_container, LV_STATE_DEFAULT);
    lv_obj_set_size(game_container, game_area_w + 2 * BORDER_WIDTH, game_area_h + 2 * BORDER_WIDTH);
    lv_obj_set_pos(game_container, game_area_x - BORDER_WIDTH, game_area_y - BORDER_WIDTH);
    lv_obj_add_event_cb(game_container, direction_event_cb, LV_EVENT_CLICKED, NULL);

    score_label = lv_label_create(screen);
    lv_obj_add_style(score_label, &style_score, LV_STATE_DEFAULT);
    lv_label_set_text_fmt(score_label, "Score: %d", score);
    lv_obj_set_align(score_label, LV_ALIGN_TOP_RIGHT);
    lv_obj_set_pos(score_label, SCORE_OFFSET_X, SCORE_OFFSET_Y);
    lv_obj_move_foreground(score_label);

    /* 方向按钮（右侧） */
    lv_coord_t btn_base_x = game_area_x + game_area_w + 2 * BORDER_WIDTH + BTN_MARGIN;
    lv_coord_t btn_base_y = screen_h / 2 - BTN_SIZE * 2 - BTN_MARGIN;

    lv_obj_t *btn_up = lv_button_create(screen);
    lv_obj_add_style(btn_up, &style_btn, LV_STATE_DEFAULT);
    lv_obj_set_pos(btn_up, btn_base_x + BTN_SIZE + BTN_MARGIN, btn_base_y);
    lv_obj_t *img_up = lv_image_create(btn_up);
    lv_obj_add_style(img_up, &style_btn_img, LV_STATE_DEFAULT);
    lv_obj_set_size(img_up, BTN_SIZE - 10, BTN_SIZE - 10);
    lv_image_set_src(img_up, "A:./up.bmp");
    lv_obj_add_event_cb(btn_up, button_dir_cb, LV_EVENT_CLICKED, (void *)DIR_UP);

    lv_obj_t *btn_down = lv_button_create(screen);
    lv_obj_add_style(btn_down, &style_btn, LV_STATE_DEFAULT);
    lv_obj_set_pos(btn_down, btn_base_x + BTN_SIZE + BTN_MARGIN, btn_base_y + BTN_SIZE + BTN_MARGIN);
    lv_obj_t *img_down = lv_image_create(btn_down);
    lv_obj_add_style(img_down, &style_btn_img, LV_STATE_DEFAULT);
    lv_obj_set_size(img_down, BTN_SIZE - 10, BTN_SIZE - 10);
    lv_image_set_src(img_down, "A:./down.bmp");
    lv_obj_add_event_cb(btn_down, button_dir_cb, LV_EVENT_CLICKED, (void *)DIR_DOWN);

    lv_obj_t *btn_left = lv_button_create(screen);
    lv_obj_add_style(btn_left, &style_btn, LV_STATE_DEFAULT);
    lv_obj_set_pos(btn_left, btn_base_x, btn_base_y + BTN_SIZE + BTN_MARGIN);
    lv_obj_t *img_left = lv_image_create(btn_left);
    lv_obj_add_style(img_left, &style_btn_img, LV_STATE_DEFAULT);
    lv_obj_set_size(img_left, BTN_SIZE - 10, BTN_SIZE - 10);
    lv_image_set_src(img_left, "A:./left.bmp");
    lv_obj_add_event_cb(btn_left, button_dir_cb, LV_EVENT_CLICKED, (void *)DIR_LEFT);

    lv_obj_t *btn_right = lv_button_create(screen);
    lv_obj_add_style(btn_right, &style_btn, LV_STATE_DEFAULT);
    lv_obj_set_pos(btn_right, btn_base_x + BTN_SIZE * 2 + BTN_MARGIN * 2, btn_base_y + BTN_SIZE + BTN_MARGIN);
    lv_obj_t *img_right = lv_image_create(btn_right);
    lv_obj_add_style(img_right, &style_btn_img, LV_STATE_DEFAULT);
    lv_obj_set_size(img_right, BTN_SIZE - 10, BTN_SIZE - 10);
    lv_image_set_src(img_right, "A:./right.bmp");
    lv_obj_add_event_cb(btn_right, button_dir_cb, LV_EVENT_CLICKED, (void *)DIR_RIGHT);

    /* 右下角暂停 / 重来按钮 */
    lv_coord_t pause_reset_btn_x = screen_w - PAUSE_RESET_BTN_SIZE - PAUSE_RESET_MARGIN;
    lv_coord_t pause_reset_btn_y = screen_h - PAUSE_RESET_BTN_SIZE - PAUSE_RESET_MARGIN;

    reset_btn = lv_button_create(screen);
    lv_obj_add_style(reset_btn, &style_btn, LV_STATE_DEFAULT);
    lv_obj_set_size(reset_btn, PAUSE_RESET_BTN_SIZE, PAUSE_RESET_BTN_SIZE);
    lv_obj_set_pos(reset_btn, pause_reset_btn_x - PAUSE_RESET_BTN_SIZE - BTN_SPACING, pause_reset_btn_y);
    reset_img = lv_image_create(reset_btn);
    lv_obj_add_style(reset_img, &style_btn_img, LV_STATE_DEFAULT);
    lv_obj_set_size(reset_img, PAUSE_RESET_IMG_SIZE, PAUSE_RESET_IMG_SIZE);
    lv_image_set_src(reset_img, "A:./reset.bmp");
    lv_obj_add_event_cb(reset_btn, pause_reset_btn_cb, LV_EVENT_CLICKED, (void *)1);

    pause_btn = lv_button_create(screen);
    lv_obj_add_style(pause_btn, &style_btn, LV_STATE_DEFAULT);
    lv_obj_set_size(pause_btn, PAUSE_RESET_BTN_SIZE, PAUSE_RESET_BTN_SIZE);
    lv_obj_set_pos(pause_btn, pause_reset_btn_x, pause_reset_btn_y);
    pause_img = lv_image_create(pause_btn);
    lv_obj_add_style(pause_img, &style_btn_img, LV_STATE_DEFAULT);
    lv_obj_set_size(pause_img, PAUSE_RESET_IMG_SIZE, PAUSE_RESET_IMG_SIZE);
    lv_image_set_src(pause_img, "A:./pause.bmp");
    lv_obj_add_event_cb(pause_btn, pause_reset_btn_cb, LV_EVENT_CLICKED, (void *)0);

    /* 创建定时器 */
    move_timer = lv_timer_create(move_snake, MOVE_INTERVAL, NULL);
    if (move_timer == NULL) {
        printf("Error: 定时器创建失败！\n");
        return;
    }
    lv_timer_pause(move_timer);
    reset_game();
}

/*************************
 * 网络线程（方向 + 暂停 + 重来）
 *************************/
static void *network_thread_func(void *arg) {
    (void)arg;
    net_sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (net_sockfd == -1) {
        perror("[网络] socket error");
        return NULL;
    }
    int opt = 1;
    setsockopt(net_sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(NET_PORT);
    if (inet_pton(AF_INET, NET_IP, &server_addr.sin_addr) <= 0) {
        perror("[网络] inet_pton error");
        close(net_sockfd);
        return NULL;
    }

    if (bind(net_sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        perror("[网络] bind error");
        close(net_sockfd);
        return NULL;
    }

    if (listen(net_sockfd, 5) == -1) {
        perror("[网络] listen error");
        close(net_sockfd);
        return NULL;
    }

    printf("[网络] 控制服务已启动，监听 %s:%d\n", NET_IP, NET_PORT);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(net_sockfd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd == -1) {
            if (errno == EINTR) continue;
            perror("[网络] accept error");
            break;
        }
        printf("[网络] 新客户端连接：%s:%d\n",
               inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

        char c;
        while (1) {
            int ret = recv(client_fd, &c, 1, 0);
            if (ret <= 0) {
                if (ret == 0) printf("[网络] 客户端断开连接。\n");
                else perror("[网络] recv error");
                break;
            }

            /* 解析指令 */
            snake_dir_t new_dir = DIR_NONE;
            switch (c) {
                case 'U': case 'u': new_dir = DIR_UP; break;
                case 'D': case 'd': new_dir = DIR_DOWN; break;
                case 'L': case 'l': new_dir = DIR_LEFT; break;
                case 'R': case 'r': new_dir = DIR_RIGHT; break;
                case 'P': case 'p': /* 暂停切换 */
                    pthread_mutex_lock(&game_mutex);
                    if (!game_over) {
                        net_pause_toggle = true;   /* 请求主线程切换暂停 */
                        printf("[网络] 收到暂停切换指令\n");
                    } else {
                        printf("[网络] 游戏已结束，忽略暂停指令\n");
                    }
                    pthread_mutex_unlock(&game_mutex);
                    break;
                case 'Z': case 'z': /* 重来 */
                    net_reset_request = true;
                    printf("[网络] 收到重来指令\n");
                    break;
                default:
                    printf("[网络] 收到无效指令：%c\n", c);
                    break;
            }

            if (new_dir != DIR_NONE) {
                pthread_mutex_lock(&game_mutex);
                if (!game_over && !game_paused) {
                    /* 防止掉头 */
                    if ((curr_dir == DIR_UP && new_dir != DIR_DOWN) ||
                        (curr_dir == DIR_DOWN && new_dir != DIR_UP) ||
                        (curr_dir == DIR_LEFT && new_dir != DIR_RIGHT) ||
                        (curr_dir == DIR_RIGHT && new_dir != DIR_LEFT)) {
                        next_dir = new_dir;
                        printf("[网络] 收到方向指令：%c -> 更新方向\n", c);
                    } else {
                        printf("[网络] 收到方向指令：%c (忽略，不能掉头)\n", c);
                    }
                } else {
                    printf("[网络] 游戏状态不允许改变方向 (game_over=%d, game_paused=%d)\n",
                           game_over, game_paused);
                }
                pthread_mutex_unlock(&game_mutex);
            }
        }
        close(client_fd);
    }
    close(net_sockfd);
    return NULL;
}

void start_network_control(void) {
    if (pthread_create(&net_thread, NULL, network_thread_func, NULL) != 0) {
        perror("[网络] pthread_create error");
        exit(1);
    }
    pthread_detach(net_thread);
}
