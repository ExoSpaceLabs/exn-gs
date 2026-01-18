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

# CCSDSPack integration placeholder:
# Option A: git submodule at external/CCSDSPack -> add_subdirectory(external/CCSDSPack)
# Option B: FetchContent -> declare here
