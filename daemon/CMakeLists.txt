add_executable(exo_gsd
  src/main.cpp
  src/app.cpp
  src/serial_link.cpp
  src/framer.cpp
  src/state.cpp
  src/storage_file.cpp
  src/ipc_server.cpp
)

target_include_directories(exo_gsd PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(exo_gsd PRIVATE
  exogs_shared
  Boost::system
)

exogs_apply_warnings(exo_gsd)
