cmake_minimum_required(VERSION 3.15)
project(profiler-cpp LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set (CMAKE_CXX_STANDARD 23)

# 将 cmake 文件夹添加到模块搜索路径
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# ----------------------------------- 配置依赖库 ---------------------------------- #
# 引入 blazesym 配置
include(SetupBlazesym)

# 引入 eBPF 配置
find_package(BpfObject REQUIRED)

# ------------------------------- 构建目标 ------------------------------- #
# 编译 BPF 对象并生成 profiler_skel
bpf_object(profiler bpf/profiler.bpf.c)

add_executable(profiler src/main.cpp)

# 链接 blazesym 和生成的 BPF skeleton
target_link_directories(profiler PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/build")
target_link_libraries(profiler 
    PRIVATE
    blazesym
    profiler_skel
)
