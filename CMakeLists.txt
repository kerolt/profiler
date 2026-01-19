cmake_minimum_required(VERSION 3.15)
project(profiler-cpp LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ------------------------ 下载并加载 Corrosion (Rust 桥接工具) ----------------------- #
include(FetchContent)
FetchContent_Declare(
    Corrosion
    GIT_REPOSITORY https://github.com/corrosion-rs/corrosion.git
    GIT_TAG v0.6.0
)
FetchContent_MakeAvailable(Corrosion)

# -------------------------- 引入 submodule 中的 Rust 项目 ------------------------- #
corrosion_import_crate(
    MANIFEST_PATH third_party/blazesym/Cargo.toml
    NO_AUTO_EXE
)

# 查找系统依赖（符合跨平台规范）
find_package(Threads REQUIRED)         # 对应 -lpthread
find_library(LIB_RT rt)                # 对应 -lrt
find_library(LIB_DL dl)                # 对应 -ldl
find_library(LIB_M m)                  # 对应 -lm

# 定义导入目标
add_library(blazesym STATIC IMPORTED GLOBAL)

# 设置库文件路径与头文件
set_target_properties(blazesym PROPERTIES
    IMPORTED_LOCATION "${CMAKE_CURRENT_SOURCE_DIR}/build/libblazesym_c.a"
    INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/third_party/blazesym/capi/include"
)

# 将官方要求的系统库绑定到 blazesym 目标
# 这样任何链接 blazesym 的项目都会自动传递这些依赖，无需重复书写
set_target_properties(blazesym PROPERTIES
    INTERFACE_LINK_LIBRARIES "Threads::Threads;${LIB_RT};${LIB_DL};${LIB_M}"
)

# ------------------------------- 配置自己的 C++ target ------------------------------- #
add_executable(profiler main.cpp)

# 链接 blazesym (Corrosion 会自动找到生成的 libblazesym_c.a)
target_link_libraries(profiler PRIVATE blazesym)
