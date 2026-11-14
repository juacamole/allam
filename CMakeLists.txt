cmake_minimum_required(VERSION 3.20)
project(allam VERSION 0.4.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_path(HTTPLIB_INCLUDE_DIR httplib.h REQUIRED)
find_package(nlohmann_json REQUIRED)

add_executable(allam
    src/main.cpp
    src/config.cpp
    src/server.cpp
    src/daemon.cpp
    src/client.cpp
)
target_compile_options(allam PRIVATE -Wall -Wextra -Wpedantic)
target_include_directories(allam PRIVATE ${HTTPLIB_INCLUDE_DIR})
target_link_libraries(allam PRIVATE nlohmann_json::nlohmann_json)

install(TARGETS allam)
