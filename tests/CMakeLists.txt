include(FetchContent)
FetchContent_Declare(googletest URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip)
FetchContent_MakeAvailable(googletest)

find_package(Threads REQUIRED)
include(GoogleTest)

foreach(t url_test http_response_test http_client_test html_links_test crawler_test async_crawler_test async_http_test)
    add_executable(${t} ${t}.cpp)
    target_link_libraries(${t} PRIVATE mc_core GTest::gtest_main Threads::Threads)
    gtest_discover_tests(${t})
endforeach()

# 需要真实MySQL的集成测试（没设置MC_TEST_DB时自动跳过）
add_executable(mysql_store_test mysql_store_test.cpp)
target_link_libraries(mysql_store_test PRIVATE mc_storage GTest::gtest_main Threads::Threads)
gtest_discover_tests(mysql_store_test)