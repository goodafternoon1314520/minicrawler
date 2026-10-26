cmake_minimum_required(VERSION 3.16)
project(minicrawler LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 现在多线程，ThreadSanitizer用来抓取数据竞争（不能和ASan同时开）
option(MC_TSAN "Build with ThreadSanitizer" OFF)

# 开启警告，并在Debug下启用AddressSanitizer抓内存问题
add_compile_options(-Wall -Wextra -Wpedantic)
if (MC_TSAN)
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=thread)
elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

find_package(Threads REQUIRED)

# 核心库：模块存放，测试和主程序共用
add_library(mc_core
        src/util/url.cpp
        src/net/http_client.cpp
        src/net/http_response.cpp
        src/net/event_loop.cpp
        src/net/async_http.cpp
        src/parser/html_links.cpp
        src/crawler/crawler.cpp
        src/crawler/async_crawler.cpp
        src/crawler/storage_worker.cpp
)
target_include_directories(mc_core PUBLIC src)
target_link_libraries(mc_core PUBLIC Threads::Threads)

# 存储层：依赖MySQL客户端库
find_package(PkgConfig REQUIRED)
pkg_check_modules(MYSQL REQUIRED IMPORTED_TARGET mysqlclient)

add_library(mc_storage
    src/storage/mysql_db.cpp
    src/storage/mysql_store.cpp
)
target_link_libraries(mc_storage PUBLIC mc_core PkgConfig::MYSQL)

add_executable(minicrawler src/main.cpp)
target_link_libraries(minicrawler PRIVATE mc_core mc_storage)

# 测试
enable_testing()
add_subdirectory(tests)