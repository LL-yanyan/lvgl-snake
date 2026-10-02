#
# ARM Linux 开发板（aarch64）交叉编译工具链
#
# 用法（在仓库根目录）：
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=aarch64-linux-gnu.toolchain.cmake
#   cmake --build build -j
#
# 注意：下方编译器路径需按虚拟机 / 主机中的实际安装位置修改。
#
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER   /usr/local/arm/gcc-linaro-7.5.0-2019.12-x86_64_aarch64-linux-gnu/bin/aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER /usr/local/arm/gcc-linaro-7.5.0-2019.12-x86_64_aarch64-linux-gnu/bin/aarch64-linux-gnu-g++)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
