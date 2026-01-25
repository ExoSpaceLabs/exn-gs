add_executable(gs_test gs_test.cpp)
target_link_libraries(gs_test PRIVATE exn_shared)
target_include_directories(gs_test PRIVATE ${CMAKE_SOURCE_DIR}/shared/include)
