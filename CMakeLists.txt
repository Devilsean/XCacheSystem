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

#搜索目录下的所有.cpp源文件
file(GLOB SOURCES "*.cpp")

#设置目标可执行文件
add_executable(main ${SOURCES})
