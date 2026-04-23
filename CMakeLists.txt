cmake_minimum_required(VERSION 3.16)
project(custom-k8s-scheduler VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(CURL REQUIRED)

include(FetchContent)
FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
)
FetchContent_MakeAvailable(nlohmann_json)

add_executable(scheduler
    src/main.cpp
    src/http_client.cpp
    src/k8s_client.cpp
    src/scheduler.cpp
)

target_include_directories(scheduler PRIVATE src)

target_link_libraries(scheduler
    PRIVATE
        CURL::libcurl
        nlohmann_json::nlohmann_json
)

target_compile_options(scheduler PRIVATE -Wall -Wextra -Wpedantic)
