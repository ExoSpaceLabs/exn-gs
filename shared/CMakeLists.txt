add_library(exn_shared
  src/protocol.cpp
  src/time.cpp
  src/framer.cpp
  src/ccsds.cpp
)

target_include_directories(exn_shared PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(exn_shared PUBLIC
  Boost::system
  ccsdspack::CCSDSPack
)

exn_apply_warnings(exn_shared)
