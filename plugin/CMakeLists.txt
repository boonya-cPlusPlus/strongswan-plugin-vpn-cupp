# 修改内容：支持 STRONGSWAN_SRC 源码头文件模式（Ubuntu 20.04 无 strongswan-dev 包） 修改人：pengjunlin 时间：2026-08-04 22:40:00 -- start ----
# 修改内容：创建插件构建脚本 修改人：pengjunlin 时间：2026-08-04 16:42:14 -- start ----
find_package(PkgConfig REQUIRED)

pkg_check_modules(YAML REQUIRED yaml-0.1)

# ---------------------------------------------------------------------------
# strongswan 头文件 + 库定位：两种模式
#   模式 A（默认）：pkg_check_modules strongswan（发行版有 libstrongswan-dev 包时）
#   模式 B（STRONGSWAN_SRC）：cmake -DSTRONGSWAN_SRC=/path/strongswan-5.8.2
#       头文件直接用源码目录，链接用系统已装好的运行库
#       适用场景：Ubuntu 20.04 focal 源里没有 libstrongswan-dev 包
# ---------------------------------------------------------------------------
if(DEFINED STRONGSWAN_SRC AND EXISTS "${STRONGSWAN_SRC}/src/libstrongswan/library.h")
    set(STRONGSWAN_MODE "SRC")
    message(STATUS "CUPP: strongswan mode: SRC (headers from ${STRONGSWAN_SRC})")
    # strongswan 源码的头文件布局：
    #   #include <library.h>         → src/libstrongswan/library.h
    #   #include <daemon.h>          → src/libcharon/daemon.h
    #   #include <utils/debug.h>       → src/libstrongswan/utils/debug.h
    #   #include <attributes/attribute_provider.h> → src/libcharon/attributes/attribute_provider.h
    include_directories(
        "${STRONGSWAN_SRC}/src/libstrongswan"
        "${STRONGSWAN_SRC}/src/libcharon"
        "${STRONGSWAN_SRC}/src"
    )
    # 链接库：Ubuntu 20.04 系统已装好的 libstrongswan.so 运行库
    find_library(STRONGSWAN_LIB
        NAMES strongswan
        PATHS /usr/lib/x86_64-linux-gnu /usr/lib /usr/local/lib
        NO_DEFAULT_PATH
    )
    if(NOT STRONGSWAN_LIB)
        find_library(STRONGSWAN_LIB NAMES strongswan)
    endif()
    if(NOT STRONGSWAN_LIB)
        message(FATAL_ERROR "CUPP: 找不到 libstrongswan.so，先 apt install libstrongswan")
    endif()
    set(STRONGSWAN_LIBRARIES "${STRONGSWAN_LIB}")
    set(STRONGSWAN_LIBRARY_DIRS "")
else()
    set(STRONGSWAN_MODE "PKG")
    message(STATUS "CUPP: strongswan mode: PKG (pkg-config)")
    pkg_check_modules(STRONGSWAN REQUIRED strongswan)
    include_directories(${STRONGSWAN_INCLUDE_DIRS})
endif()

include_directories(
    ${YAML_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

set(CUPP_SOURCES
    src/plugin.c
    src/event_listener.c
    src/identity_manager.c
    src/policy_engine.c
    src/yaml_loader.c
    src/ip_allocator.c
    src/fixed_pool.c
    src/dynamic_pool.c
    src/session_manager.c
)

add_library(charon-user-policy SHARED ${CUPP_SOURCES})

# strongswan plugin_loader 按文件名 libstrongswan-<name>.so 查找（libstrongswan- 前缀是
# 所有插件——无论 libstrongswan 还是 libcharon 插件——的统一约定）。
# 插件名 user-policy → .so: libstrongswan-user-policy.so
# 构造符号：<name>_plugin_create（连字符转下划线）→ user_policy_plugin_create
set_target_properties(charon-user-policy PROPERTIES
    OUTPUT_NAME "strongswan-user-policy"
    PREFIX "lib"
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)

target_compile_options(charon-user-policy PRIVATE
    -Wall -Wextra -Wno-unused-parameter
)

target_link_libraries(charon-user-policy
    ${STRONGSWAN_LIBRARIES}
    ${YAML_LIBRARIES}
)
if(STRONGSWAN_LIBRARY_DIRS)
    target_link_directories(charon-user-policy PRIVATE
        ${STRONGSWAN_LIBRARY_DIRS}
        ${YAML_LIBRARY_DIRS}
    )
else()
    target_link_directories(charon-user-policy PRIVATE
        ${YAML_LIBRARY_DIRS}
    )
endif()
# 修改内容：支持 STRONGSWAN_SRC 源码头文件模式（Ubuntu 20.04 无 strongswan-dev 包） 修改人：pengjunlin 时间：2026-08-04 22:40:00 -- end ----

install(TARGETS charon-user-policy
    LIBRARY DESTINATION ${PLUGIN_DIR}
)

# 安装示例配置
install(FILES ../config/policy.yaml
    DESTINATION /etc/strongswan/cupp
)
# 修改内容：创建插件构建脚本 修改人：pengjunlin 时间：2026-08-04 16:42:14 -- end ----
