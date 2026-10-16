include(FetchContent)
FetchContent_Declare(googletest URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip)
FetchContent_MakeAvailable(googletest)

find_package(Threads REQUIRED)
include(GoogleTest)

foreach(t url_test http_response_test http_client_test)
    add_executable(${t} ${t}.cpp)
    target_link_libraries(${t} PRIVATE mc_core GTest::gtest_main Threads::Threads)
    gtest_discover_tests(${t})
endforeach()