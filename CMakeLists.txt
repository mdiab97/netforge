cmake_minimum_required(VERSION 3.16)
project(NetForge
    VERSION 1.0.0
    DESCRIPTION "High-performance event-loop networking library for game servers"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(NETFORGE_BUILD_TESTS "Build unit tests" ON)
option(NETFORGE_BUILD_EXAMPLES "Build example programs" ON)
option(NETFORGE_ENABLE_TLS "Enable TLS support via OpenSSL" OFF)

find_package(Threads REQUIRED)

if(NETFORGE_ENABLE_TLS)
    find_package(OpenSSL REQUIRED)
endif()

add_library(netforge
    src/transport.cpp
    src/connection.cpp
    src/message.cpp
    src/server.cpp
    src/client.cpp
    src/event_loop.cpp
    src/buffer_pool.cpp
    src/crypto/tls_context.cpp
)

target_include_directories(netforge
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(netforge
    PUBLIC
        Threads::Threads
)

if(NETFORGE_ENABLE_TLS)
    target_link_libraries(netforge PUBLIC OpenSSL::SSL OpenSSL::Crypto)
    target_compile_definitions(netforge PUBLIC NETFORGE_TLS_ENABLED)
endif()

if(WIN32)
    target_link_libraries(netforge PUBLIC ws2_32 mswsock)
endif()

target_compile_options(netforge PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>
)

if(NETFORGE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(NETFORGE_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

install(TARGETS netforge
    EXPORT NetForgeTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(DIRECTORY include/netforge
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(EXPORT NetForgeTargets
    FILE NetForgeTargets.cmake
    NAMESPACE NetForge::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/NetForge
)

configure_package_config_file(
    cmake/NetForgeConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/NetForgeConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/NetForge
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/NetForgeConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/NetForgeConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/NetForgeConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/NetForge
)
