

add_executable(exn_gsd
  src/main.cpp
  src/app.cpp
  src/serial_link.cpp
  src/state.cpp
  src/storage_file.cpp
  src/ipc_server.cpp
)

target_include_directories(exn_gsd PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(exn_gsd PRIVATE
  exn_shared
  Boost::system
)

exn_apply_warnings(exn_gsd)
