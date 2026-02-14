include(FetchContent)

# Boost (prefer system). On Debian/Ubuntu:
#   sudo apt-get install libboost-dev libboost-system-dev
# Force legacy FindBoost behavior to avoid partial Boost CMake configs.
set(Boost_NO_BOOST_CMAKE ON)
find_package(Boost REQUIRED COMPONENTS system)

# FTXUI for the UI. This FetchContent requires internet at configure time.
# If you want offline builds, vendor FTXUI under external/ftxui and use add_subdirectory.
FetchContent_Declare(
  ftxui
  GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
  GIT_TAG v5.0.0
)
FetchContent_MakeAvailable(ftxui)

# CCSDSPack integration
# Automatically build and install CCSDSPack if not found on the system.
# Note: CCSDSPack's CMakeLists.txt uses CMAKE_SOURCE_DIR, which causes issues
# when added via add_subdirectory. We build it as an ExternalProject.
include(ExternalProject)

find_package(CCSDSPack QUIET)
if(NOT CCSDSPack_FOUND)
  message(STATUS "CCSDSPack not found. Building and using internal copy...")
  ExternalProject_Add(ccsdspack_build
    SOURCE_DIR /home/dev/Works/CCSDSPack
    CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=${CMAKE_BINARY_DIR}/external_install
               -DENABLE_TESTER=OFF
               -DENABLE_ENCODER=OFF
               -DENABLE_DECODER=OFF
               -DENABLE_VALIDATOR=OFF
               -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
               -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
    BUILD_BYPRODUCTS ${CMAKE_BINARY_DIR}/external_install/lib/libccsdspack.so
  )
  
  set(CCSDSPACK_INSTALL_DIR ${CMAKE_BINARY_DIR}/external_install)
  
  file(MAKE_DIRECTORY ${CCSDSPACK_INSTALL_DIR}/include)
  
  add_library(ccsdspack_lib SHARED IMPORTED GLOBAL)
  set_target_properties(ccsdspack_lib PROPERTIES
    IMPORTED_LOCATION ${CCSDSPACK_INSTALL_DIR}/lib/libccsdspack.so
    INTERFACE_INCLUDE_DIRECTORIES ${CCSDSPACK_INSTALL_DIR}/include
  )
  add_dependencies(ccsdspack_lib ccsdspack_build)
  
  # Alias it to match the find_package target name
  add_library(ccsdspack::CCSDSPack ALIAS ccsdspack_lib)
else()
  message(STATUS "Found CCSDSPack: ${CCSDSPack_DIR}")
endif()
