include(FetchContent)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
)
FetchContent_MakeAvailable(googletest)

add_executable(test_shared_region test_shared_region.cpp)
target_link_libraries(test_shared_region PRIVATE ipc GTest::gtest_main)
add_test(NAME SharedRegionTests COMMAND test_shared_region)