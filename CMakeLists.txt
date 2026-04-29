cmake_minimum_required(VERSION 3.14)
project(backend_visual)

set(CMAKE_CXX_STANDARD 17)

set(THIRD_PARTY_DIR ${PROJECT_SOURCE_DIR}/third_party)
set(SRC_DIR ${PROJECT_SOURCE_DIR}/VsCode)
set(INCLUDE_DIR ${PROJECT_SOURCE_DIR}/include)

find_package(SDL2 REQUIRED)
find_package(OpenGL REQUIRED)
find_package(GLEW REQUIRED)
find_package(Threads REQUIRED)
find_package(CURL REQUIRED)

find_package(cpr CONFIG)
if(NOT cpr_FOUND)
    message(STATUS "CPR not found, will build from source")
    include(FetchContent)
    FetchContent_Declare(
        cpr
        GIT_REPOSITORY https://github.com/libcpr/cpr.git
        GIT_TAG 1.10.5
    )
    FetchContent_MakeAvailable(cpr)
endif()

if(NOT EXISTS "${THIRD_PARTY_DIR}/stb/stb_image.h")
    file(DOWNLOAD 
        "https://raw.githubusercontent.com/nothings/stb/master/stb_image.h"
        "${THIRD_PARTY_DIR}/stb/stb_image.h"
        SHOW_PROGRESS
    )
endif()

add_library(imgui STATIC
    ${THIRD_PARTY_DIR}/imgui/imgui.cpp
    ${THIRD_PARTY_DIR}/imgui/imgui_draw.cpp
    ${THIRD_PARTY_DIR}/imgui/imgui_demo.cpp
    ${THIRD_PARTY_DIR}/imgui/imgui_tables.cpp
    ${THIRD_PARTY_DIR}/imgui/imgui_widgets.cpp
    ${THIRD_PARTY_DIR}/imgui/backends/imgui_impl_sdl2.cpp
    ${THIRD_PARTY_DIR}/imgui/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC 
    ${THIRD_PARTY_DIR}/imgui 
    ${THIRD_PARTY_DIR}/imgui/backends
    ${SDL2_INCLUDE_DIRS}
)
target_compile_options(imgui PRIVATE -fPIC)

add_library(implot STATIC
    ${THIRD_PARTY_DIR}/implot/implot.cpp
    ${THIRD_PARTY_DIR}/implot/implot_demo.cpp
    ${THIRD_PARTY_DIR}/implot/implot_items.cpp
)
target_include_directories(implot PUBLIC ${THIRD_PARTY_DIR}/implot)
target_link_libraries(implot PRIVATE imgui)
target_compile_options(implot PRIVATE -fPIC)


add_executable(telemetry_monitor 
    ${SRC_DIR}/main.cpp
    ${SRC_DIR}/parsers.cpp
    ${SRC_DIR}/backend.cpp
    ${SRC_DIR}/gui.cpp
)

target_include_directories(telemetry_monitor PRIVATE 
    ${SRC_DIR}
    ${THIRD_PARTY_DIR}/stb
)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.0)
    target_link_libraries(telemetry_monitor PRIVATE stdc++fs)
endif()

target_link_libraries(telemetry_monitor PRIVATE 
    imgui 
    implot 
    zmq 
    ${SDL2_LIBRARIES} 
    ${OPENGL_LIBRARIES} 
    ${GLEW_LIBRARIES} 
    Threads::Threads
    ${CURL_LIBRARIES}
    dl
)

# Линкуем CPR
if(TARGET cpr::cpr)
    target_link_libraries(telemetry_monitor PRIVATE cpr::cpr)
elseif(TARGET cpr)
    target_link_libraries(telemetry_monitor PRIVATE cpr)
endif()