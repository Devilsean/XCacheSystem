#设置最低cmake版本
cmake_minimum_required(VERSION 3.10)

#设置项目名称
project(XCacheSystem)

#设置C++标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

#启用调试模式
set(CMAKE_BUILD_TYPE Debug)
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -O0")

# 搜索目录下的所有.cpp源文件，但排除测试文件
file(GLOB SOURCES "*.cpp")
list(REMOVE_ITEM SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/cache_test.cpp")
list(REMOVE_ITEM SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/testAllCachePolicy.cpp")

#设置主程序可执行文件
if(SOURCES)
    add_executable(main ${SOURCES})
endif()

# 直接使用 FetchContent 下载 GTest，避免系统版本冲突
include(FetchContent)
FetchContent_Declare(
    googletest
    URL https://codeload.github.com/google/googletest/zip/refs/tags/v1.14.0
)
FetchContent_MakeAvailable(googletest)

# 创建测试可执行文件
add_executable(cache_test cache_test.cpp)
target_link_libraries(cache_test GTest::gtest GTest::gtest_main)

# 创建 testAllCachePolicy 可执行文件
add_executable(testAllCachePolicy testAllCachePolicy.cpp)

