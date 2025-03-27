cmake_minimum_required(VERSION 3.22)
project(coroutine-in-c C)

set(CMAKE_C_STANDARD 11)

add_compile_options(-U_FORTIFY_SOURCE)

add_library(coroutine SHARED src/coroutine.c)
target_include_directories(coroutine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_executable(naive tests/naive.c)
target_link_libraries(naive PRIVATE coroutine)

add_executable(producer-consumer tests/producer-consumer.c)
target_link_libraries(producer-consumer PRIVATE coroutine)

add_executable(resume-test tests/resume-test.c)
target_link_libraries(resume-test PRIVATE coroutine)