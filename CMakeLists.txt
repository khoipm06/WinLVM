cmake_minimum_required(VERSION 3.25)

project(winlvm VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(WINLVM_BUILD_TESTS "Build winlvm tests" ON)

add_library(winlvm_core
  src/lvmcore/activation.cpp
  src/lvmcore/activation_policy.cpp
  src/lvmcore/block_mapper.cpp
  src/lvmcore/crc32.cpp
  src/lvmcore/device_enum.cpp
  src/lvmcore/exporter.cpp
  src/lvmcore/hash.cpp
  src/lvmcore/image_reader.cpp
  src/lvmcore/json.cpp
  src/lvmcore/metadata_parser.cpp
  src/lvmcore/scanner.cpp
  src/lvmcore/stripe_support.cpp
  src/lvmcore/volume_lock.cpp
  src/lvmcore/winbtrfs_integration.cpp
)

target_include_directories(winlvm_core
  PUBLIC
    src/common/include
    src/lvmcore/include
)

if(WIN32)
  target_link_libraries(winlvm_core PUBLIC bcrypt)
endif()

if(MSVC)
  target_compile_options(winlvm_core PRIVATE /W4 /permissive-)
else()
  target_compile_options(winlvm_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

set(WINLVM_COMMANDS
  lvm
  pvs
  vgs
  lvs
  pvscan
  vgscan
  lvscan
  pvdisplay
  vgdisplay
  lvdisplay
  lvchange
  vgchange
  lvcreate
  lvremove
  lvresize
  vgextend
  vgreduce
  pvcreate
  pvremove
)

foreach(command_name IN LISTS WINLVM_COMMANDS)
  add_executable(${command_name} src/cli/main.cpp)
  target_link_libraries(${command_name} PRIVATE winlvm_core)
  set_target_properties(${command_name} PROPERTIES OUTPUT_NAME ${command_name})
  if(MSVC)
    target_compile_options(${command_name} PRIVATE /W4 /permissive-)
  else()
    target_compile_options(${command_name} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endforeach()

add_executable(lvm2d src/service/lvm2d.cpp)
target_link_libraries(lvm2d PRIVATE winlvm_core)
if(MSVC)
  target_compile_options(lvm2d PRIVATE /W4 /permissive-)
else()
  target_compile_options(lvm2d PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(CTest)
if(WINLVM_BUILD_TESTS AND BUILD_TESTING)
  add_executable(winlvm_tests tests/winlvm_tests.cpp)
  target_link_libraries(winlvm_tests PRIVATE winlvm_core)
  if(MSVC)
    target_compile_options(winlvm_tests PRIVATE /W4 /permissive-)
  else()
    target_compile_options(winlvm_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME winlvm_tests COMMAND winlvm_tests)
endif()
