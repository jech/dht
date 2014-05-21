cmake_minimum_required(VERSION 2.8)
project(dht)

option(DHT_BUILD_EXAMPLE "Build DHT example program" ON)

add_library(dht STATIC dht.c dht.h)
target_include_directories(dht PUBLIC ${PROJECT_SOURCE_DIR})

if(DHT_BUILD_EXAMPLE)
    add_executable(dht-example dht-example.c)
    target_link_libraries(dht-example dht crypt)
endif()
