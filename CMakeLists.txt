cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(pm-vfs
    VERSION 0.1.0
    DESCRIPTION "PolyMech VFS jail and CLI"
    LANGUAGES CXX)
endif()

if(NOT TARGET CLI11::CLI11 OR NOT TARGET nlohmann_json::nlohmann_json)
  include(FetchContent)
  # Reuse deps already fetched by the Tanit build when this package is configured alone.
  set(_pm_vfs_deps "${CMAKE_CURRENT_SOURCE_DIR}/../../build/release-debug/_deps")
  if(NOT TARGET CLI11::CLI11)
    if(NOT DEFINED FETCHCONTENT_SOURCE_DIR_CLI11
       AND EXISTS "${_pm_vfs_deps}/cli11-src/CMakeLists.txt")
      set(FETCHCONTENT_SOURCE_DIR_CLI11 "${_pm_vfs_deps}/cli11-src" CACHE PATH "" FORCE)
    endif()
    set(CLI11_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(CLI11_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(cli11
      GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
      GIT_TAG v2.4.2
      GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(cli11)
  endif()
  if(NOT TARGET nlohmann_json::nlohmann_json)
    if(NOT DEFINED FETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON
       AND EXISTS "${_pm_vfs_deps}/nlohmann_json-src/CMakeLists.txt")
      set(FETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON "${_pm_vfs_deps}/nlohmann_json-src" CACHE PATH "" FORCE)
    endif()
    set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(nlohmann_json
      GIT_REPOSITORY https://github.com/nlohmann/json.git
      GIT_TAG v3.11.3
      GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(nlohmann_json)
  endif()
endif()

add_library(pm-vfs-lib STATIC
  src/commons/path_sanitizer.cpp
  src/error.cpp
  src/path.cpp
  src/config.cpp
  src/store.cpp
  src/copy.cpp
  src/tasks.cpp)
target_include_directories(pm-vfs-lib
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
          "${CMAKE_CURRENT_SOURCE_DIR}/src/commons")
target_compile_features(pm-vfs-lib PUBLIC cxx_std_20)
target_link_libraries(pm-vfs-lib PUBLIC nlohmann_json::nlohmann_json)
if(MSVC)
  target_compile_definitions(pm-vfs-lib PRIVATE
    _CRT_SECURE_NO_WARNINGS
    _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING)
  target_compile_options(pm-vfs-lib PRIVATE /W3 /utf-8 /EHsc)
else()
  target_compile_options(pm-vfs-lib PRIVATE -Wall -Wextra)
endif()

include(FetchContent)
set(_pm_vfs_deps "${CMAKE_CURRENT_SOURCE_DIR}/../../build/release-debug/_deps")
if(NOT TARGET httplib::httplib)
  if(NOT DEFINED FETCHCONTENT_SOURCE_DIR_CPP_HTTPLIB
     AND EXISTS "${_pm_vfs_deps}/cpp_httplib-src/httplib.h")
    set(FETCHCONTENT_SOURCE_DIR_CPP_HTTPLIB "${_pm_vfs_deps}/cpp_httplib-src" CACHE PATH "" FORCE)
  endif()
  set(HTTPLIB_USE_ZLIB_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
  set(HTTPLIB_REQUIRE_ZLIB OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(cpp_httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG v0.16.3
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(cpp_httplib)
endif()

add_executable(pm-vfs cli/main.cpp src/http.cpp)
target_link_libraries(pm-vfs PRIVATE pm-vfs-lib CLI11::CLI11 nlohmann_json::nlohmann_json httplib::httplib)
target_compile_features(pm-vfs PRIVATE cxx_std_20)
if(MSVC)
  target_compile_definitions(pm-vfs PRIVATE _CRT_SECURE_NO_WARNINGS)
  target_compile_options(pm-vfs PRIVATE /W3 /utf-8 /EHsc)
endif()
