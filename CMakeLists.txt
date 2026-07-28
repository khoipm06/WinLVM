cmake_minimum_required(VERSION 3.24)
project(winlvm LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8 /Zc:__cplusplus)
else()
  add_compile_options(-Wall -Wextra -Wconversion -Wsign-conversion)
  # Self-contained binaries: a MinGW/msys2 toolchain otherwise produces
  # executables that need libstdc++/libgcc/libwinpthread on PATH, which fails
  # with a bare exit 127 when run outside the msys2 shell.
  if(WIN32)
    add_link_options(-static -static-libgcc -static-libstdc++)
  endif()
endif()

# ---------------------------------------------------------------------------
# lvmfmt — LVM2 on-disk format. Platform-agnostic by construction: no Win32,
# no CRT beyond <cstdint>/<string>/<vector>/<optional>, so the same sources can
# later build for kernel mode and UEFI. Keep it that way.
# ---------------------------------------------------------------------------
add_library(lvmfmt STATIC
  src/lvmfmt/crc32.cpp
  src/lvmfmt/source.cpp
  src/lvmfmt/config.cpp
  src/lvmfmt/label.cpp
  src/lvmfmt/metadata.cpp
  src/lvmfmt/mapping.cpp
)
target_include_directories(lvmfmt PUBLIC src/lvmfmt/include)

# ---------------------------------------------------------------------------
# lvmdump — inspect an image and print its computed device-mapper table.
# ---------------------------------------------------------------------------
add_executable(lvmdump src/tools/lvmdump/main.cpp)
target_link_libraries(lvmdump PRIVATE lvmfmt)

# ---------------------------------------------------------------------------
# Corpus. Ground truth lives in corpus/ as zstd-compressed images; tests need
# them decompressed. Only the non-LUKS images are unpacked — LUKS containers
# are opaque until the luksfmt layer exists (roadmap Phase 5).
# ---------------------------------------------------------------------------
set(CORPUS_SRC "${CMAKE_CURRENT_SOURCE_DIR}/corpus")
set(CORPUS_OUT "${CMAKE_BINARY_DIR}/corpus-images")

find_program(ZSTD_EXE zstd)
file(GLOB CORPUS_ZST "${CORPUS_SRC}/images/*.img.zst")

set(CORPUS_IMAGES "")
if(ZSTD_EXE)
  file(MAKE_DIRECTORY "${CORPUS_OUT}")
  foreach(zst ${CORPUS_ZST})
    get_filename_component(zname "${zst}" NAME)
    string(REGEX REPLACE "\\.zst$" "" iname "${zname}")
    if(iname MATCHES "^luks2-")
      continue()
    endif()
    set(out "${CORPUS_OUT}/${iname}")
    add_custom_command(
      OUTPUT "${out}"
      COMMAND "${ZSTD_EXE}" -d -q -f --sparse "${zst}" -o "${out}"
      DEPENDS "${zst}"
      COMMENT "Decompressing corpus image ${iname}"
      VERBATIM)
    list(APPEND CORPUS_IMAGES "${out}")
  endforeach()
  add_custom_target(corpus ALL DEPENDS ${CORPUS_IMAGES})
else()
  message(WARNING "zstd not found: corpus oracle tests will be skipped.")
endif()

# ---------------------------------------------------------------------------
# Driver parser test — validates the freestanding C parser matches lvmfmt.
# ---------------------------------------------------------------------------
if(ZSTD_EXE)
  add_executable(winlvm_driver_parser_tests
    tests/lvm_read/test_driver_parser.c
    src/driver/lvm_read.c
  )
  target_link_libraries(winlvm_driver_parser_tests PRIVATE zstd)
  target_include_directories(winlvm_driver_parser_tests PRIVATE src/driver)
  if(TARGET corpus)
    add_dependencies(winlvm_driver_parser_tests corpus)
  endif()
  # Pass the raw corpus directory so the test can decompress images directly
  add_test(NAME winlvm_driver_parser_tests COMMAND winlvm_driver_parser_tests "${CORPUS_SRC}")
endif()
enable_testing()

add_executable(winlvm_tests
  tests/test_main.cpp
  tests/test_units.cpp
  tests/test_corpus.cpp
)
target_link_libraries(winlvm_tests PRIVATE lvmfmt)
target_compile_definitions(winlvm_tests PRIVATE
  WINLVM_CORPUS_EXPECTED="${CORPUS_SRC}/expected"
  WINLVM_CORPUS_IMAGES="${CORPUS_OUT}"
)
if(TARGET corpus)
  add_dependencies(winlvm_tests corpus)
endif()

add_test(NAME winlvm_tests COMMAND winlvm_tests)
