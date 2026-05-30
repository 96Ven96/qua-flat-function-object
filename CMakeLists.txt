cmake_minimum_required(VERSION 3.16)
project(OpcuaFunctionObject VERSION 1.0 LANGUAGES CXX)

# ──────────────────────────────────────────────────────────────────────────────
# TemplateHelper companion toolkit (sibling directory)
# ──────────────────────────────────────────────────────────────────────────────
if (NOT TARGET TemplateHelper)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../TemplateHelper" TemplateHelper_build)
endif()

# ──────────────────────────────────────────────────────────────────────────────
# Qt — auto-detects Qt6 (preferred) or Qt5
# ──────────────────────────────────────────────────────────────────────────────
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core)

# ──────────────────────────────────────────────────────────────────────────────
# QUaServer — OPC UA library
#
# This project does NOT manage QUaServer for you. Configure it before adding
# this subdirectory using whichever method fits your workflow:
#
#   add_subdirectory(/path/to/QUaServer)
#   find_package(QUaServer REQUIRED)
#   FetchContent_MakeAvailable(QUaServer)
#   # vcpkg / Conan toolchain files handle it automatically
#
# As a convenience, a silent find_package is attempted first so that an
# installed system package is picked up without any extra boilerplate.
# ──────────────────────────────────────────────────────────────────────────────
if (NOT TARGET QUaServer)
    find_package(QUaServer QUIET)
endif()
if (NOT TARGET QUaServer)
    message(FATAL_ERROR
        "QUaServer target not found.\n"
        "Configure it before adding this subdirectory — see the comment in "
        "opcua_function/CMakeLists.txt for the supported options."
    )
endif()

# ──────────────────────────────────────────────────────────────────────────────
# OpcuaFunctionObject — compiled STATIC library (has Q_OBJECT, needs MOC)
#
# Usage:
#   target_link_libraries(MyTarget PRIVATE OpcuaFunctionObject::OpcuaFunctionObject)
# ──────────────────────────────────────────────────────────────────────────────
add_library(OpcuaFunctionObject STATIC
    opcua_function_object.cpp
    opcua_function_object.h
)
add_library(OpcuaFunctionObject::OpcuaFunctionObject ALIAS OpcuaFunctionObject)

set_target_properties(OpcuaFunctionObject PROPERTIES AUTOMOC ON)

target_compile_features(OpcuaFunctionObject PUBLIC cxx_std_20)
target_include_directories(OpcuaFunctionObject PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# TemplateHelper::Qt already propagates Qt::Core transitively.
# Adjust QUaServer to the actual CMake target name exposed by your OPC UA library.
target_link_libraries(OpcuaFunctionObject PUBLIC
    TemplateHelper::Qt
    QUaServer
)
