# 修改内容：创建顶层 CMake 构建入口 修改人：pengjunlin 时间：2026-08-04 16:42:14 -- start ----
cmake_minimum_required(VERSION 3.10)

project(charon-user-policy C)

# C11 标准
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# 默认安装前缀，可通过 -DCMAKE_INSTALL_PREFIX 覆盖
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "/usr" CACHE PATH "Install prefix" FORCE)
endif()

# 插件 .so 安装目录，可通过 -DPLUGIN_DIR 覆盖
if(NOT DEFINED PLUGIN_DIR)
    set(PLUGIN_DIR "${CMAKE_INSTALL_PREFIX}/lib/ipsec/plugins" CACHE PATH "strongSwan plugin dir")
endif()

add_subdirectory(plugin)
# 修改内容：创建顶层 CMake 构建入口 修改人：pengjunlin 时间：2026-08-04 16:42:14 -- end ----
