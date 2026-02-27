cmake_minimum_required(VERSION 3.14)
project(imgui_project)

set(CMAKE_CXX_STANDARD 17)

set(THIRD_PARTY_DIR ${PROJECT_SOURCE_DIR}/third_party)
set(SRC_DIR ${PROJECT_SOURCE_DIR}/src)
set(INCLUDE_DIR ${PROJECT_SOURCE_DIR}/include)

# ---------------- SDL2 ----------------
find_package(SDL2 REQUIRED)
include_directories(${SDL2_INCLUDE_DIRS})

# ---------------- OpenGL / GLEW ----------------
find_package(OpenGL REQUIRED)
find_package(GLEW REQUIRED)

include_directories(${INCLUDE_DIR})

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
    ${SDL2_LIBRARIES}
    ${OPENGL_LIBRARIES}
    GLEW::GLEW
)

# ---------------- IMPLOT ----------------
add_library(implot STATIC
    ${THIRD_PARTY_DIR}/implot/implot.cpp
    ${THIRD_PARTY_DIR}/implot/implot_items.cpp
    ${THIRD_PARTY_DIR}/implot/implot_demo.cpp
)

target_include_directories(implot PUBLIC
    ${THIRD_PARTY_DIR}/implot
)

target_link_libraries(implot PUBLIC imgui)

# ---------------- MAIN ----------------
add_executable(main
    ${SRC_DIR}/main.cpp
)

# ---------------- ZMQ + JSON ----------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(ZMQ REQUIRED libzmq)

find_package(nlohmann_json REQUIRED)

target_include_directories(main PRIVATE
    ${ZMQ_INCLUDE_DIRS}
)

target_link_libraries(main PRIVATE
    imgui
    implot
    ${SDL2_LIBRARIES}
    ${OPENGL_LIBRARIES}
    GLEW::GLEW
    ${ZMQ_LIBRARIES}
    nlohmann_json::nlohmann_json
)
