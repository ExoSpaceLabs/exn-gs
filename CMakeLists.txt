cmake_minimum_required(VERSION 3.20)
project(exo_gs LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(EXOGS_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)

include(cmake/warnings.cmake)
include(cmake/deps.cmake)

add_subdirectory(shared)
add_subdirectory(daemon)
add_subdirectory(ui)
