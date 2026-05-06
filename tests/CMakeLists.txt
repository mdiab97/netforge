include(FetchContent)

FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.5.2
)
FetchContent_MakeAvailable(Catch2)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(CTest)
include(Catch)

add_executable(netforge_tests
    test_message.cpp
    test_buffer.cpp
    test_spsc_queue.cpp
    test_buffer_pool.cpp
    test_server_client.cpp
    test_crypto.cpp
)

target_link_libraries(netforge_tests PRIVATE
    netforge
    Catch2::Catch2WithMain
)

catch_discover_tests(netforge_tests)

# Stress test is a separate executable (long-running)
add_executable(netforge_stress_test
    test_stress.cpp
)

target_link_libraries(netforge_stress_test PRIVATE
    netforge
    Catch2::Catch2WithMain
)

catch_discover_tests(netforge_stress_test)
