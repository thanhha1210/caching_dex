cmake_minimum_required(VERSION 3.10)
project(caching_dex CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

add_definitions(-UNDEBUG)

add_compile_options(
    -Wall
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -Wno-sign-compare
)

include_directories(
    ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/include
)

add_executable(newbench test/newbench.cpp)
target_link_libraries(newbench PRIVATE pthread)
target_compile_options(newbench PRIVATE -O2)
