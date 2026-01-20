# 1. 加载 Corrosion
include(FetchContent)
FetchContent_Declare(
    Corrosion
    GIT_REPOSITORY https://github.com/corrosion-rs/corrosion.git
    GIT_TAG v0.6.0
)
FetchContent_MakeAvailable(Corrosion)

# 2. 导入 Rust 项目
corrosion_import_crate(
    MANIFEST_PATH ${CMAKE_CURRENT_SOURCE_DIR}/third_party/blazesym/Cargo.toml
    NO_AUTO_EXE
)

# 3. 查找必要的系统链接库
find_package(Threads REQUIRED)
find_library(LIB_RT rt)
find_library(LIB_DL dl)
find_library(LIB_M m)

# 4. 定义并配置静态库目标
add_library(blazesym STATIC IMPORTED GLOBAL)

# 注意：使用 CMAKE_CURRENT_SOURCE_DIR 确保路径正确
set_target_properties(blazesym PROPERTIES
    IMPORTED_LOCATION "${CMAKE_CURRENT_BINARY_DIR}/libblazesym_c.a" # 建议指向编译输出目录
    INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/third_party/blazesym/capi/include"
    INTERFACE_LINK_LIBRARIES "Threads::Threads;${LIB_RT};${LIB_DL};${LIB_M}"
)