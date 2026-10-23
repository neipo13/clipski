cmake_minimum_required(VERSION 3.20)
project(clipski LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# 1. OpenGL (opengl32 on Windows)
find_package(OpenGL REQUIRED)

# 2. Fetch or Find SDL2
find_package(SDL2 CONFIG QUIET)
if(NOT SDL2_FOUND)
    message(STATUS "Downloading SDL2 prebuilt VC development libraries...")
    FetchContent_Declare(
        sdl2_prebuilt
        URL "https://github.com/libsdl-org/SDL/releases/download/release-2.30.8/SDL2-devel-2.30.8-VC.zip"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(sdl2_prebuilt)
    set(SDL2_DIR "${sdl2_prebuilt_SOURCE_DIR}/cmake")
    find_package(SDL2 CONFIG REQUIRED)
endif()

# 3. Fetch or Find Dear ImGui
find_package(imgui CONFIG QUIET)
if(NOT imgui_FOUND)
    message(STATUS "Fetching Dear ImGui v1.91.4...")
    FetchContent_Declare(
        imgui_src
        URL "https://github.com/ocornut/imgui/archive/refs/tags/v1.91.4.zip"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(imgui_src)

    add_library(imgui STATIC
        "${imgui_src_SOURCE_DIR}/imgui.cpp"
        "${imgui_src_SOURCE_DIR}/imgui_draw.cpp"
        "${imgui_src_SOURCE_DIR}/imgui_tables.cpp"
        "${imgui_src_SOURCE_DIR}/imgui_widgets.cpp"
        "${imgui_src_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp"
        "${imgui_src_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp"
    )
    target_include_directories(imgui PUBLIC
        "${imgui_src_SOURCE_DIR}"
        "${imgui_src_SOURCE_DIR}/backends"
    )
    target_link_libraries(imgui PUBLIC SDL2::SDL2 OpenGL::GL)
    add_library(imgui::imgui ALIAS imgui)
endif()

# 4. Find libmpv
find_path(MPV_INCLUDE_DIR NAMES mpv/client.h mpv/render_gl.h
    HINTS "${MPV_ROOT}/include" "${CMAKE_SOURCE_DIR}/external/mpv/include"
)
find_library(MPV_LIBRARY NAMES mpv libmpv libmpv-2
    HINTS "${MPV_ROOT}" "${MPV_ROOT}/lib" "${CMAKE_SOURCE_DIR}/external/mpv/lib" "${CMAKE_SOURCE_DIR}/external/mpv"
)

if(NOT MPV_INCLUDE_DIR OR NOT MPV_LIBRARY)
    message(FATAL_ERROR
        "Could not find libmpv in external/mpv! Run the build script or place libmpv dev files in external/mpv."
    )
endif()

add_library(mpv_target UNKNOWN IMPORTED)
set_target_properties(mpv_target PROPERTIES
    IMPORTED_LOCATION "${MPV_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${MPV_INCLUDE_DIR}"
)

# Executable target (WIN32 prevents the black command prompt window from appearing)
add_executable(clipski WIN32 main.cpp clipski.rc)

target_link_libraries(clipski PRIVATE
    SDL2::SDL2
    imgui::imgui
    OpenGL::GL
    mpv_target
    shell32
)

if(MSVC)
    target_compile_options(clipski PRIVATE /W4 /utf-8)
    target_link_options(clipski PRIVATE /ENTRY:mainCRTStartup)
endif()

# Copy runtime DLLs (SDL2.dll and libmpv-2.dll) next to clipski.exe automatically
if(EXISTS "${sdl2_prebuilt_SOURCE_DIR}/lib/x64/SDL2.dll")
    add_custom_command(TARGET clipski POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${sdl2_prebuilt_SOURCE_DIR}/lib/x64/SDL2.dll"
            "$<TARGET_FILE_DIR:clipski>/SDL2.dll"
    )
endif()

if(EXISTS "${CMAKE_SOURCE_DIR}/external/mpv/libmpv-2.dll")
    add_custom_command(TARGET clipski POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/external/mpv/libmpv-2.dll"
            "$<TARGET_FILE_DIR:clipski>/libmpv-2.dll"
    )
endif()

