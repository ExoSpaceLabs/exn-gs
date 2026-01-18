add_executable(exo_gsui
  src/main.cpp
  src/ui_app.cpp
  src/ipc_client.cpp
)

target_include_directories(exo_gsui PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(exo_gsui PRIVATE
  exogs_shared
  Boost::system
  ftxui::screen
  ftxui::dom
  ftxui::component
)

exogs_apply_warnings(exo_gsui)
