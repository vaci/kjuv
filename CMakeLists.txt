cmake_minimum_required(VERSION 3.10)

include(GoogleTest)

project(kjuv LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

find_package(CapnProto REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(GTest REQUIRED)

find_package(PkgConfig REQUIRED)

pkg_check_modules(LIBUV REQUIRED IMPORTED_TARGET libuv)

add_library(kjuv STATIC
  src/kjuv.cpp
)

include_directories(kjuv PRIVATE
  CapnProto::capnp
  PkgConfig::LIBUV
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
  PkgConfig::LIBUV
  GTest::gtest
)

gtest_discover_tests(kjuv-test)

install(FILES src/kjuv.h DESTINATION include)
install(
  TARGETS kjuv
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
)
