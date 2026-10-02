cmake_minimum_required(VERSION 3.16)

#
# 交叉编译到 ARM Linux 开发板时，在配置阶段指定工具链文件：
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=aarch64-linux-gnu.toolchain.cmake
#
# 在本机 Linux（x86_64）直接编译则无需指定：
#   cmake -B build
#
project(lvgl_snake LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# 让 LVGL 使用本仓库根目录下的 lv_conf.h（无需向 lvgl 源码树复制配置）
set(LV_CONF_PATH
    ${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h
    CACHE STRING "Path to the lv_conf.h used by LVGL" FORCE)

# LVGL 以 git 子模块形式位于 ./lvgl（克隆时请加 --recurse-submodules）
add_subdirectory(lvgl)
add_subdirectory(src)
