cmake_minimum_required(VERSION 3.10)

include(GoogleTest)

project(kjuv LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

find_package(CapnProto REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(GTest REQUIRED)

add_library(kjuv
  src/kjuv.cpp
)

set(LIBUV_INCLUDE_DIRS "$ENV{LIBUV_DIR}/include")
set(LIBUV_LIBRARY "$ENV{LIBUV_DIR}/lib/libuv.so")

target_include_directories(kjuv PRIVATE
  ${CAPNP_INCLUDE_DIRS}
  ${LIBUV_INCLUDE_DIRS}
)

add_executable(kjuv-test
  src/kjuv-test.cpp
)

target_link_libraries(kjuv-test PRIVATE
  kjuv
  CapnProto::capnp
  CapnProto::kj-async
  CapnProto::kj
  OpenSSL::SSL
  ${LIBUV_LIBRARY}
  GTest::gtest
)

gtest_discover_tests(kjuv-test)
