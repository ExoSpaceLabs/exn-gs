add_executable(exn_gsui
  src/main.cpp
  src/ui_app.cpp
  src/ipc_client.cpp
)

target_include_directories(exn_gsui PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(exn_gsui PRIVATE
  exn_shared
  Boost::system
  ftxui::screen
  ftxui::dom
  ftxui::component
)

exn_apply_warnings(exn_gsui)
