cmake_minimum_required(VERSION 3.22...3.30)

project(sv-multicam-replay
    VERSION 1.0.0
    DESCRIPTION "SV MultiCam Replay Plugin for OBS Studio"
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Enable Qt Automoc, Autouic, Autorcc
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# Find Qt 6 (Standard for OBS Studio 30, 31, 32)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

# Find libobs & obs-frontend-api
# Can be provided via -DOBS_DIR="..." or standard find_package if OBS SDK is installed
if(NOT TARGET obs-frontend-api OR NOT TARGET libobs)
    find_package(libobs REQUIRED)
    find_package(obs-frontend-api REQUIRED)
endif()

# Source files
set(SOURCES
    src/plugin-main.cpp
    src/multicam-manager.cpp
    src/multicam-manager.hpp
    src/source-buffer.cpp
    src/source-buffer.hpp
    src/mp4-muxer.cpp
    src/mp4-muxer.hpp
    src/ui/multicam-dock.cpp
    src/ui/multicam-dock.hpp
)

# Create plugin shared library
add_library(sv-multicam-replay MODULE ${SOURCES})

target_include_directories(sv-multicam-replay PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui
)

target_link_libraries(sv-multicam-replay PRIVATE
    libobs
    obs-frontend-api
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
)

# Compiler definitions & options
target_compile_definitions(sv-multicam-replay PRIVATE
    UNICODE
    _UNICODE
    SV_PLUGIN_VERSION="${PROJECT_VERSION}"
)

if(MSVC)
    target_compile_options(sv-multicam-replay PRIVATE
        /W4
        /MP
        /utf-8
    )
endif()

# Set output name and properties for OBS plugins
set_target_properties(sv-multicam-replay PROPERTIES
    PREFIX ""
    OUTPUT_NAME "sv-multicam-replay"
)

# Installation targets for OBS Studio
if(WIN32)
    install(TARGETS sv-multicam-replay
        RUNTIME DESTINATION "obs-plugins/64bit"
        LIBRARY DESTINATION "obs-plugins/64bit"
    )
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/data")
        install(DIRECTORY "data/"
            DESTINATION "data/obs-plugins/sv-multicam-replay"
        )
    endif()
endif()
