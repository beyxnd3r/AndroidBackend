cmake_minimum_required(VERSION 3.14)
project(imgui_project)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Определите PROJECT_SOURCE_DIR если не определен
if(NOT PROJECT_SOURCE_DIR)
    set(PROJECT_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
endif()

find_package(OpenGL REQUIRED)

set(THIRD_PARTY_DIR ${PROJECT_SOURCE_DIR}/third_party)
set(SRC_DIR ${PROJECT_SOURCE_DIR}/src)
set(INCLUDE_DIR ${PROJECT_SOURCE_DIR}/include)

# ---------------- SDL2 ----------------
find_package(SDL2 REQUIRED)
include_directories(${SDL2_INCLUDE_DIRS})

# ---------------- OpenGL уже найден выше ----------------

# ---------------- PkgConfig / GLEW / ZMQ ----------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(GLEW REQUIRED glew)
pkg_check_modules(ZMQ REQUIRED libzmq)
pkg_check_modules(CURL REQUIRED libcurl)

include_directories(${GLEW_INCLUDE_DIRS})
include_directories(${INCLUDE_DIR})

# ---------------- PostgreSQL ----------------
find_package(PostgreSQL REQUIRED)
include_directories(${PostgreSQL_INCLUDE_DIRS})

# ---------------- IMGUI ----------------
add_library(imgui STATIC
    ${THIRD_PARTY_DIR}/imgui/imgui.cpp
    ${THIRD_PARTY_DIR}/imgui/imgui_draw.cpp
    ${THIRD_PARTY_DIR}/imgui/imgui_tables.cpp
    ${THIRD_PARTY_DIR}/imgui/imgui_widgets.cpp
    ${THIRD_PARTY_DIR}/imgui/imgui_demo.cpp
    ${THIRD_PARTY_DIR}/imgui/backends/imgui_impl_sdl2.cpp
    ${THIRD_PARTY_DIR}/imgui/backends/imgui_impl_opengl3.cpp
)

target_include_directories(imgui PUBLIC
    ${THIRD_PARTY_DIR}/imgui
    ${THIRD_PARTY_DIR}/imgui/backends
)

target_link_libraries(imgui PUBLIC
    SDL2::SDL2
    ${OPENGL_LIBRARIES}
    ${GLEW_LIBRARIES}
)

# ---------------- IMPLOT ----------------
add_library(implot STATIC
    ${THIRD_PARTY_DIR}/implot/implot.cpp
    ${THIRD_PARTY_DIR}/implot/implot_items.cpp
)

target_include_directories(implot PUBLIC
    ${THIRD_PARTY_DIR}/implot
)

target_link_libraries(implot PUBLIC imgui)

# ---------------- JSON ----------------
find_package(nlohmann_json REQUIRED)

# ---------------- Source Files ----------------
set(SOURCES
    ${SRC_DIR}/main.cpp
    ${SRC_DIR}/config.cpp
    ${SRC_DIR}/database.cpp
    ${SRC_DIR}/json_parser.cpp
    ${SRC_DIR}/heatmap.cpp
    ${SRC_DIR}/gps_tracking.cpp
    ${SRC_DIR}/gui.cpp
    ${SRC_DIR}/server.cpp
    ${SRC_DIR}/texture_utils.cpp
)

# ---------------- MAIN EXECUTABLE ----------------
add_executable(network_monitor ${SOURCES})

target_include_directories(network_monitor PRIVATE
    ${INCLUDE_DIR}
    ${ZMQ_INCLUDE_DIRS}
    ${PostgreSQL_INCLUDE_DIRS}
    ${CURL_INCLUDE_DIRS}
    ${OPENGL_INCLUDE_DIRS}
)

target_compile_definitions(network_monitor PRIVATE
    PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)

target_link_libraries(network_monitor PRIVATE
    imgui
    implot
    SDL2::SDL2
    ${OPENGL_LIBRARIES}
    ${GLEW_LIBRARIES}
    ${ZMQ_LIBRARIES}
    ${PostgreSQL_LIBRARIES}
    ${CURL_LIBRARIES}
    nlohmann_json::nlohmann_json
    pthread
    dl
)

# ---------------- STB Image ----------------
target_include_directories(network_monitor PRIVATE
    ${THIRD_PARTY_DIR}
)