add_executable(gs_test gs_test.cpp)
target_link_libraries(gs_test PRIVATE exn_shared)
target_include_directories(gs_test PRIVATE ${CMAKE_SOURCE_DIR}/shared/include)
exn_apply_warnings(gs_test)

add_test(NAME exn_gs_packet_regression COMMAND gs_test)

add_test(
  NAME exn_gs_hil_smoke
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/hil_smoke.sh ${CMAKE_BINARY_DIR}
)
set_tests_properties(exn_gs_hil_smoke PROPERTIES TIMEOUT 30)
