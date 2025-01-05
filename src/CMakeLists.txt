set(kjuv_sources
  kjuv.cpp
  )

set(kjuv_headers
  kjuv.h
  )

set(kjuv-test_sources
  kjuv-test.cpp
)

add_library(kjuv ${kjuv_sources})
add_library(kjuv-test ${kjuv-test_sources})



