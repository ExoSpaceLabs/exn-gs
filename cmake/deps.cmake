include(FetchContent)

# Boost (prefer system). On Debian/Ubuntu:
#   sudo apt-get install libboost-dev libboost-system-dev
set(Boost_NO_BOOST_CMAKE ON)
find_package(Boost REQUIRED COMPONENTS system)

# FTXUI for the terminal UI.
FetchContent_Declare(
  ftxui
  GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
  GIT_TAG v5.0.0
)
FetchContent_MakeAvailable(ftxui)

# CCSDSPack v2 integration.
# Prefer an installed compatible 2.x package. For clean checkouts, build the
# released v2.0.0 source in isolation and consume its installed package target.
include(ExternalProject)

find_package(CCSDSPack 2.0 CONFIG QUIET)
if(CCSDSPack_FOUND AND TARGET ccsdspack::CCSDSPack)
  message(STATUS "Found CCSDSPack ${CCSDSPack_VERSION}: ${CCSDSPack_DIR}")
else()
  set(CCSDSPACK_VERSION v2.0.0)
  set(CCSDSPACK_INSTALL_DIR ${CMAKE_BINARY_DIR}/external_install)

  message(STATUS "CCSDSPack 2.x not found. Building ${CCSDSPACK_VERSION} from the released source...")
  ExternalProject_Add(ccsdspack_build
    GIT_REPOSITORY https://github.com/ExoSpaceLabs/CCSDSPack.git
    GIT_TAG ${CCSDSPACK_VERSION}
    GIT_SHALLOW TRUE
    UPDATE_DISCONNECTED TRUE
    CMAKE_ARGS
      -DCMAKE_INSTALL_PREFIX=${CCSDSPACK_INSTALL_DIR}
      -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
      -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
      -DENABLE_TESTER=OFF
      -DENABLE_ENCODER=OFF
      -DENABLE_DECODER=OFF
      -DENABLE_VALIDATOR=OFF
      -DCCSDSPACK_ENABLE_FUZZING=OFF
      -DCCSDSPACK_BUILD_MCU=OFF
    BUILD_BYPRODUCTS ${CCSDSPACK_INSTALL_DIR}/lib/libccsdspack.so
  )

  file(MAKE_DIRECTORY ${CCSDSPACK_INSTALL_DIR}/include)

  add_library(ccsdspack_v2 SHARED IMPORTED GLOBAL)
  set_target_properties(ccsdspack_v2 PROPERTIES
    IMPORTED_LOCATION ${CCSDSPACK_INSTALL_DIR}/lib/libccsdspack.so
    INTERFACE_INCLUDE_DIRECTORIES ${CCSDSPACK_INSTALL_DIR}/include
  )
  add_dependencies(ccsdspack_v2 ccsdspack_build)
  add_library(ccsdspack::CCSDSPack ALIAS ccsdspack_v2)
endif()
