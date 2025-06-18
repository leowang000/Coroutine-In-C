cmake_minimum_required(VERSION 3.22)
project(coroutine-in-c C)

set(CMAKE_C_STANDARD 11)

add_compile_options(-U_FORTIFY_SOURCE)

add_library(coroutine SHARED src/coroutine.c)
target_include_directories(coroutine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

file(GLOB TEST_SOURCES "tests/*.c")
foreach(test_source ${TEST_SOURCES})
    # 从文件路径获取文件名（不带扩展名）
    get_filename_component(test_name ${test_source} NAME_WE)
    add_executable(${test_name} ${test_source})
    target_link_libraries(${test_name} PRIVATE coroutine)
endforeach()