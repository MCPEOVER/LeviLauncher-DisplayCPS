cmake_minimum_required(VERSION 3.18)
project(DisplayCPS LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# STL 일관성
set(CMAKE_ANDROID_STL_TYPE c++_shared)

# Optimization flags
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -fvisibility=hidden -ffunction-sections -fdata-sections -fexceptions -w")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -fvisibility=hidden -ffunction-sections -fdata-sections -w")
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--gc-sections,--strip-all -s")

include(FetchContent)

# ============================
# fmt
# ============================
FetchContent_Declare(
    fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG 10.2.1
)
FetchContent_MakeAvailable(fmt)

# ============================
# preloader-android
# ============================
FetchContent_Declare(
    preloader_android
    GIT_REPOSITORY https://github.com/LiteLDev/preloader-android.git
    GIT_TAG main
)

FetchContent_GetProperties(preloader_android)
if(NOT preloader_android_POPULATED)
    FetchContent_Populate(preloader_android)

    # Logger.h 패치
    file(READ "${preloader_android_SOURCE_DIR}/src/pl/Logger.h" LOGGER_CONTENT)
    string(REPLACE "#include <format>" "#include <fmt/format.h>" LOGGER_CONTENT "${LOGGER_CONTENT}")
    string(REPLACE "std::vformat" "fmt::vformat" LOGGER_CONTENT "${LOGGER_CONTENT}")
    string(REPLACE "std::make_format_args" "fmt::make_format_args" LOGGER_CONTENT "${LOGGER_CONTENT}")
    file(WRITE "${preloader_android_SOURCE_DIR}/src/pl/Logger.h" "${LOGGER_CONTENT}")

    # 🚨 GlossHook 미리 빌드된거 쓰지 말고, 직접 빌드
    set(PRELOADER_BUILD_GLOSSHOOK ON CACHE BOOL "" FORCE)

    add_subdirectory(${preloader_android_SOURCE_DIR} ${preloader_android_BINARY_DIR})
endif()

# libc++ shared
find_library(ANDROID_CXX_LIB c++_shared)

# preloader 링크
target_link_libraries(preloader PUBLIC fmt::fmt ${ANDROID_CXX_LIB})

# include paths
include_directories(
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/ImGui
    ${CMAKE_SOURCE_DIR}/src/ImGui/backends
)

set(SOURCES
    src/main.cpp
    src/ImGui/imgui.cpp
    src/ImGui/imgui_draw.cpp
    src/ImGui/imgui_tables.cpp
    src/ImGui/imgui_widgets.cpp
    src/ImGui/backends/imgui_impl_opengl3.cpp
    src/ImGui/backends/imgui_impl_android.cpp
)

add_library(DisplayCPS SHARED ${SOURCES})

target_link_libraries(DisplayCPS
    preloader
    log
    android
    EGL
    GLESv3
    GLESv2
    ${ANDROID_CXX_LIB}
)
