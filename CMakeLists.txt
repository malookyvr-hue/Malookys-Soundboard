cmake_minimum_required(VERSION 3.18)
project(Malookys_Soundboard CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Dobby (inline hook library) is fetched from GitHub at build time -- nothing to upload for it.
include(FetchContent)
set(DOBBY_GENERATE_SHARED OFF CACHE BOOL "" FORCE)
set(DOBBY_DEBUG OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  dobby
  GIT_REPOSITORY https://github.com/jmpews/Dobby.git
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(dobby)

add_library(Malookys_Soundboard SHARED src/Malookys_Soundboard.cpp)

target_include_directories(Malookys_Soundboard PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/include)

target_compile_options(Malookys_Soundboard PRIVATE -fvisibility=hidden -O2)

target_link_libraries(Malookys_Soundboard PRIVATE
    dobby_static log dl aaudio GLESv3 EGL android)
target_link_options(Malookys_Soundboard PRIVATE -Wl,--exclude-libs,ALL)
