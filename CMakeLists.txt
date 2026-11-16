cmake_minimum_required(VERSION 3.20)
project(DeepDive VERSION 1.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets OpenGL OpenGLWidgets Multimedia)

add_executable(DeepDive
    src/main.cpp
    src/GpuCapabilities.h
    src/Noise.h
    src/Shaders.h
    src/WorldGenerator.h
    src/WorldGenerator.cpp
    src/UnderwaterAudio.h
    src/UnderwaterAudio.cpp
    src/DeepDiveWidget.h
    src/DeepDiveWidget.cpp
)

target_include_directories(DeepDive PRIVATE src)

target_link_libraries(DeepDive PRIVATE
    Qt6::Widgets
    Qt6::OpenGL
    Qt6::OpenGLWidgets
    Qt6::Multimedia
)

if(WIN32)
    target_link_libraries(DeepDive PRIVATE dxgi)
endif()
