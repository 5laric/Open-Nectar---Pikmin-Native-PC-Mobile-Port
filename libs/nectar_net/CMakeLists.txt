cmake_minimum_required(VERSION 3.16)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(nectar_net VERSION 0.1.0 LANGUAGES CXX)
    include(CTest)
    set(_NECTAR_NET_TESTS_DEFAULT ON)
else()
    set(_NECTAR_NET_TESTS_DEFAULT OFF)
endif()

add_library(nectar_net STATIC
    src/byte_io.cpp
    src/protocol.cpp
)

target_compile_features(nectar_net PUBLIC cxx_std_17)
target_include_directories(nectar_net PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

if(MSVC)
    target_compile_options(nectar_net PRIVATE /W4 /WX)
else()
    target_compile_options(nectar_net PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

option(NECTAR_NET_BUILD_TESTS "Build nectar_net unit tests" ${_NECTAR_NET_TESTS_DEFAULT})
if(NECTAR_NET_BUILD_TESTS AND NOT ANDROID)
    enable_testing()
    add_executable(nectar_net_protocol_test tests/protocol_test.cpp)
    target_link_libraries(nectar_net_protocol_test PRIVATE nectar_net)
    if(MSVC)
        target_compile_options(nectar_net_protocol_test PRIVATE /W4 /WX)
    else()
        target_compile_options(nectar_net_protocol_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME nectar_net_protocol_test COMMAND nectar_net_protocol_test)
endif()
