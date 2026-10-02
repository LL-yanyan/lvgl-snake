# 贪吃蛇可执行文件
add_executable(lvgl_snake
    main.c
    snake_server.c
)

# 便于以 #include "snake_server.h" 引用
target_include_directories(lvgl_snake PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(lvgl_snake PRIVATE
    lvgl
    m
    pthread
)
