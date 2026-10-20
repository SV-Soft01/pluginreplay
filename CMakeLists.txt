cmake_minimum_required(VERSION 3.22...3.30)

project(sv-multicam-replay
    VERSION 1.0.0
    DESCRIPTION "SV MultiCam Replay Plugin for OBS Studio"
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

find_package(libobs QUIET)
find_package(obs-frontend-api QUIET)

if(NOT TARGET libobs)
    find_path(LIBOBS_INCLUDE_DIR obs.h
        PATHS
        "${CMAKE_CURRENT_SOURCE_DIR}/obs-src/libobs"
        "${CMAKE_CURRENT_SOURCE_DIR}/../obs-src/libobs"
        "${OBS_DIR}/libobs"
        "${OBS_DIR}/include"
        "C:/Program Files/obs-studio/include"
    )

    find_file(LIBOBS_LIB
        NAMES obs.lib
        PATHS
        "${CMAKE_CURRENT_SOURCE_DIR}/obs-install/bin/64bit"
        "${CMAKE_CURRENT_SOURCE_DIR}/../obs-install/bin/64bit"
        "${OBS_DIR}/bin/64bit"
        "C:/Program Files/obs-studio/bin/64bit"
    )

    add_library(libobs UNKNOWN IMPORTED)
    set_target_properties(libobs PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${LIBOBS_INCLUDE_DIR};${LIBOBS_INCLUDE_DIR}/.."
        IMPORTED_LOCATION "${LIBOBS_LIB}"
        IMPORTED_IMPLIB "${LIBOBS_LIB}"
    )
endif()

if(NOT TARGET obs-frontend-api)
    find_path(OBS_FRONTEND_INCLUDE_DIR obs-frontend-api.h
        PATHS
        "${CMAKE_CURRENT_SOURCE_DIR}/obs-src/UI/obs-frontend-api"
        "${CMAKE_CURRENT_SOURCE_DIR}/../obs-src/UI/obs-frontend-api"
        "${OBS_DIR}/UI/obs-frontend-api"
        "${OBS_DIR}/include"
        "C:/Program Files/obs-studio/include"
    )

    find_file(OBS_FRONTEND_LIB
        NAMES obs-frontend-api.lib
        PATHS
        "${CMAKE_CURRENT_SOURCE_DIR}/obs-install/bin/64bit"
        "${CMAKE_CURRENT_SOURCE_DIR}/../obs-install/bin/64bit"
        "${OBS_DIR}/bin/64bit"
        "C:/Program Files/obs-studio/bin/64bit"
    )

    add_library(obs-frontend-api UNKNOWN IMPORTED)
    set_target_properties(obs-frontend-api PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${OBS_FRONTEND_INCLUDE_DIR}"
        IMPORTED_LOCATION "${OBS_FRONTEND_LIB}"
        IMPORTED_IMPLIB "${OBS_FRONTEND_LIB}"
    )
endif()

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

set_target_properties(sv-multicam-replay PROPERTIES
    PREFIX ""
    OUTPUT_NAME "sv-multicam-replay"
)
