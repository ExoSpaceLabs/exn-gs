add_executable(exn_gsdctl main.cpp)
target_link_libraries(exn_gsdctl PRIVATE exn_shared Boost::system)
exn_apply_warnings(exn_gsdctl)
install(TARGETS exn_gsdctl RUNTIME DESTINATION bin)
