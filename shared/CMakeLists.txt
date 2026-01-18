add_library(exogs_shared
  src/protocol.cpp
  src/time.cpp
)

target_include_directories(exogs_shared PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(exogs_shared PUBLIC Boost::system)

exogs_apply_warnings(exogs_shared)
