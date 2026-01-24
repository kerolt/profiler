cmake_minimum_required(VERSION 3.19)
project(profiler-cpp LANGUAGES CXX)

# --------------------------------- 配置项目工具 -------------------------------- #
# 将 cmake 文件夹添加到模块搜索路径
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

option(ENABLE_CLANG_TIDY "Enable clang-tidy analysis" OFF)
option(ENABLE_CPPCHECK "Enable cppcheck analysis" OFF)

if(ENABLE_CLANG_TIDY)
    find_program(CLANG_TIDY_EXE NAMES "clang-tidy")
    if(CLANG_TIDY_EXE)
        set(CMAKE_CXX_CLANG_TIDY ${CLANG_TIDY_EXE} "-fix")
    else()
        message(STATUS "clang-tidy not found")
    endif()
endif()

if(ENABLE_CPPCHECK)
    find_program(CPPCHECK_EXE NAMES "cppcheck")
    if(CPPCHECK_EXE)
        set(CMAKE_CXX_CPPCHECK "${CPPCHECK_EXE}")
        message(STATUS "cppcheck enabled: ${CPPCHECK_EXE}")
    else()
        message(STATUS "cppcheck not found")
    endif()
endif()

# ----------------------------------- 配置依赖库 ---------------------------------- #
# 引入 blazesym 配置
include(SetupBlazesym)

# 引入 eBPF 配置
find_package(BpfObject REQUIRED)

find_package(CLI11 REQUIRED)
find_package(spdlog REQUIRED)

# ------------------------------- 构建目标 ------------------------------- #
# 编译 BPF 对象并生成 profiler_skel
bpf_object(profiler bpf/profiler.bpf.c)

add_executable(profiler 
    src/main.cpp
    src/event.cpp
    src/perf.cpp
)

# 链接 blazesym 和生成的 BPF skeleton
target_link_directories(profiler 
    PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}"
    src
)
target_link_libraries(profiler 
    PRIVATE
    blazesym
    profiler_skel
    CLI11::CLI11
    spdlog::spdlog
)
