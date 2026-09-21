cmake_minimum_required(VERSION 3.20)

project(PathFailoverFabric
        VERSION 1.0.0
        DESCRIPTION "Deterministic alternate path promotion runtime"
        LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(PFF_BUILD_TESTS "Build the test suites" ON)
option(PFF_BUILD_TOOLS "Build the daemon and command line tools" ON)
option(PFF_BUILD_EXAMPLES "Build the example programs" ON)
option(PFF_WARNINGS_AS_ERRORS "Promote compiler warnings to errors" ON)
option(PFF_ENABLE_ASAN "Build with AddressSanitizer" OFF)
option(PFF_ENABLE_UBSAN "Build with UndefinedBehaviorSanitizer" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------
add_library(pff
  src/authority.cpp
  src/canonical.cpp
  src/client.cpp
  src/crc32c.cpp
  src/decision.cpp
  src/fabric.cpp
  src/ids.cpp
  src/model.cpp
  src/net.cpp
  src/platform.cpp
  src/protocol.cpp
  src/records.cpp
  src/selector.cpp
  src/server.cpp
  src/status.cpp
  src/store.cpp
  src/wire.cpp
  src/net.hpp
  src/platform.hpp
)
add_library(pff::pff ALIAS pff)

target_compile_features(pff PUBLIC cxx_std_20)
target_include_directories(pff
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(pff PRIVATE Threads::Threads)
if(WIN32)
  target_link_libraries(pff PUBLIC ws2_32)
  target_compile_definitions(pff PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
endif()

if(MSVC)
  target_compile_options(pff PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc /MP)
  if(PFF_WARNINGS_AS_ERRORS)
    target_compile_options(pff PRIVATE /WX)
  endif()
else()
  target_compile_options(pff PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
  if(PFF_WARNINGS_AS_ERRORS)
    target_compile_options(pff PRIVATE -Werror)
  endif()
endif()

if(PFF_ENABLE_ASAN)
  if(MSVC)
    target_compile_options(pff PUBLIC /fsanitize=address)
  else()
    target_compile_options(pff PUBLIC -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(pff PUBLIC -fsanitize=address)
  endif()
endif()

if(PFF_ENABLE_UBSAN)
  if(NOT MSVC)
    target_compile_options(pff PUBLIC -fsanitize=undefined -fno-omit-frame-pointer)
    target_link_options(pff PUBLIC -fsanitize=undefined)
  endif()
endif()

# ---------------------------------------------------------------------------
# Tools
# ---------------------------------------------------------------------------
if(PFF_BUILD_TOOLS)
  add_executable(pffd apps/pffd.cpp)
  add_executable(pffctl apps/pffctl.cpp)
  add_executable(pff_host apps/pff_host.cpp)
  foreach(tool pffd pffctl pff_host)
    target_link_libraries(${tool} PRIVATE pff::pff)
    if(MSVC)
      target_compile_options(${tool} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc)
      if(PFF_WARNINGS_AS_ERRORS)
        target_compile_options(${tool} PRIVATE /WX)
      endif()
    else()
      target_compile_options(${tool} PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
      if(PFF_WARNINGS_AS_ERRORS)
        target_compile_options(${tool} PRIVATE -Werror)
      endif()
    endif()
  endforeach()
endif()

# ---------------------------------------------------------------------------
# Examples
# ---------------------------------------------------------------------------
if(PFF_BUILD_EXAMPLES)
  add_executable(pff_example_quickstart examples/quickstart.cpp)
  target_link_libraries(pff_example_quickstart PRIVATE pff::pff)
  if(MSVC)
    target_compile_options(pff_example_quickstart PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc)
  else()
    target_compile_options(pff_example_quickstart PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(PFF_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Install / package
# ---------------------------------------------------------------------------
install(TARGETS pff
        EXPORT pffTargets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/pff DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(PFF_BUILD_TOOLS)
  install(TARGETS pffd pffctl pff_host
          RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

install(EXPORT pffTargets
        FILE pffTargets.cmake
        NAMESPACE pff::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pff)

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/pffConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/pffConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pff)

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/pffConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
  ARCH_INDEPENDENT)

install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/pffConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/pffConfigVersion.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pff)

install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/pff)
install(FILES README.md DESTINATION ${CMAKE_INSTALL_DATADIR}/pff)
