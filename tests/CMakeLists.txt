include(FetchContent)
FetchContent_Declare(googletest URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip)
FetchContent_MakeAvailable(googletest)

add_executable(url_test url_test.cpp)
target_link_libraries(url_test
        PRIVATE
        mc_core
        GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(url_test)