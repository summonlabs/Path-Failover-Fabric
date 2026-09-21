# Test suites.
#
# Every suite is a standalone executable registered with CTest. No suite uses a
# timeout: a hanging test is a defect to diagnose, not something to hide behind a
# watchdog.

add_library(pff_test_support INTERFACE)
target_include_directories(pff_test_support INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(pff_test_support INTERFACE pff::pff)

function(pff_add_test name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE pff_test_support)
  if(MSVC)
    target_compile_options(${name} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc /MP)
    if(PFF_WARNINGS_AS_ERRORS)
      target_compile_options(${name} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${name} PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
    if(PFF_WARNINGS_AS_ERRORS)
      target_compile_options(${name} PRIVATE -Werror)
    endif()
  endif()
  add_test(NAME ${name} COMMAND ${name})
endfunction()

pff_add_test(pff_test_core test_core.cpp)
pff_add_test(pff_test_selector test_selector.cpp)
pff_add_test(pff_test_persistence test_persistence.cpp)
pff_add_test(pff_test_protocol test_protocol.cpp)
pff_add_test(pff_test_fabric test_fabric.cpp)
pff_add_test(pff_test_property test_property.cpp)
pff_add_test(pff_test_concurrency test_concurrency.cpp)
pff_add_test(pff_test_scale test_scale.cpp)
pff_add_test(pff_test_multiprocess test_multiprocess.cpp process.cpp)

target_compile_definitions(pff_test_multiprocess PRIVATE
  PFF_DAEMON_PATH="$<TARGET_FILE:pffd>"
  PFF_HOST_PATH="$<TARGET_FILE:pff_host>"
  PFF_CTL_PATH="$<TARGET_FILE:pffctl>")
