find_package(Catch2 3 REQUIRED)

add_executable(SHTTPD_tests tests.cpp)

target_link_libraries(SHTTPD_tests
    StupidHTTPDownloader
    Catch2::Catch2WithMain
    spdlog::spdlog
)

include(CTest)
include(Catch)
catch_discover_tests(SHTTPD_tests)