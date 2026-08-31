cmake_minimum_required(VERSION 3.16)

include(ExternalProject)

set(HARDRT_INSTALL_DIR ${CMAKE_BINARY_DIR}/_deps/hardrt-install)

ExternalProject_Add(hardrt_ep
        GIT_REPOSITORY https://github.com/ExoSpaceLabs/hardrt.git
        GIT_TAG main

        SOURCE_DIR  ${CMAKE_BINARY_DIR}/_deps/hardrt-src
        BINARY_DIR  ${CMAKE_BINARY_DIR}/_deps/hardrt-build
        INSTALL_DIR ${HARDRT_INSTALL_DIR}

        CMAKE_ARGS
        -DCMAKE_INSTALL_PREFIX=${HARDRT_INSTALL_DIR}
        -DHARDRT_PORT=posix
        -DHARDRT_ENABLE_CPP=ON
        -DHARDRT_BUILD_EXAMPLES=OFF
        -DHARDRT_BUILD_TESTS=OFF

        BUILD_BYPRODUCTS ${HARDRT_INSTALL_DIR}/lib/libhardrt.a
)

file(MAKE_DIRECTORY ${HARDRT_INSTALL_DIR}/include)
file(MAKE_DIRECTORY ${HARDRT_INSTALL_DIR}/lib)

add_library(hardrt STATIC IMPORTED GLOBAL)
add_dependencies(hardrt hardrt_ep)
set_target_properties(hardrt PROPERTIES
        IMPORTED_LOCATION ${HARDRT_INSTALL_DIR}/lib/libhardrt.a
)

add_executable(stm32_sim
        src/main.cpp
)

add_dependencies(stm32_sim hardrt_ep)

target_link_libraries(stm32_sim PRIVATE
        hardrt
        exn_shared
        Boost::system
)

target_include_directories(stm32_sim PRIVATE
        ${HARDRT_INSTALL_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../shared/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../daemon/include
)

exn_apply_warnings(stm32_sim)
