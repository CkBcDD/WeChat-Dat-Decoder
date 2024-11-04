cmake_minimum_required(VERSION 3.14)

project(WechatDatDecode VERSION 1.0.0)

set(CMAKE_C_STANDARD 16)

if(UNIX)
    add_definitions(-D_FILE_OFFSET_BITS=64)
endif()

file(GLOB_RECURSE SOURCES "src/main.c")

add_executable(WechatDatDecode ${SOURCES})

if(WIN32)
    target_compile_definitions(WechatDatDecode PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(WechatDatDecode PRIVATE /W4)
else()
    target_compile_options(WechatDatDecode PRIVATE -Wall -Wextra -Wpedantic)
endif()

find_package(Threads REQUIRED)
target_link_libraries(WechatDatDecode PRIVATE Threads::Threads)
