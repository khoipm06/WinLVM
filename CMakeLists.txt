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
# lvmcore — freestanding C11 LVM2 parser and arena allocator.
# Kernel-safe, zero dynamic allocations, non-recursive, stack <= 512 bytes.
# ---------------------------------------------------------------------------
add_library(lvmcore STATIC
  src/lvmcore/crc32.c
  src/lvmcore/arena.c
  src/lvmcore/parser.c
  src/lvmcore/serialize.c
)
set_target_properties(lvmcore PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
)
target_include_directories(lvmcore PUBLIC src/lvmcore/include)
if(NOT MSVC)
  target_compile_options(lvmcore PRIVATE -Wframe-larger-than=512)
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

# lvm.exe — bounded multi-call offline metadata CLI.
add_executable(lvm src/tools/lvm/main.cpp)
target_link_libraries(lvm PRIVATE lvmfmt)

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
  target_include_directories(winlvm_driver_parser_tests PRIVATE src/driver)
  if(TARGET corpus)
    add_dependencies(winlvm_driver_parser_tests corpus)
  endif()
  # Pass the raw corpus images directory; oracles come from the source tree
  add_test(NAME winlvm_driver_parser_tests
    COMMAND winlvm_driver_parser_tests "${CORPUS_OUT}" "${CORPUS_SRC}/expected")

  # Adversarial vectors for the freestanding parser: stripe-array boundary,
  # CRC edge cases, wraparound sizes, fail-closed typeless segments. Fully
  # synthetic — no corpus dependency.
  add_executable(winlvm_lvm_hardening_tests
    tests/lvm_read/test_lvm_read_hardening.c
    src/driver/lvm_read.c
  )
  target_include_directories(winlvm_lvm_hardening_tests PRIVATE src/driver)
  add_test(NAME winlvm_lvm_hardening_tests COMMAND winlvm_lvm_hardening_tests)



  # -------------------------------------------------------------------------
  # lvmcore test suite: parser, hardening, and differential verification gate.
  # -------------------------------------------------------------------------
  add_executable(test_lvmcore_parser
    tests/lvmcore/test_lvmcore_parser.c
  )
  target_link_libraries(test_lvmcore_parser PRIVATE lvmcore)
  if(TARGET corpus)
    add_dependencies(test_lvmcore_parser corpus)
  endif()
  add_test(NAME test_lvmcore_parser
    COMMAND test_lvmcore_parser "${CORPUS_OUT}" "${CORPUS_SRC}/expected")

  add_executable(test_lvmcore_hardening
    tests/lvmcore/test_lvmcore_hardening.c
  )
  target_link_libraries(test_lvmcore_hardening PRIVATE lvmcore)
  add_test(NAME test_lvmcore_hardening COMMAND test_lvmcore_hardening)

  add_executable(test_lvmcore_differential
    tests/lvmcore/test_lvmcore_differential.c
    src/driver/lvm_read.c
  )
  target_include_directories(test_lvmcore_differential PRIVATE src/driver)
  target_link_libraries(test_lvmcore_differential PRIVATE lvmcore)
  if(TARGET corpus)
    add_dependencies(test_lvmcore_differential corpus)
  endif()
  add_test(NAME test_lvmcore_differential
    COMMAND test_lvmcore_differential "${CORPUS_OUT}")

  add_executable(test_lvmcore_serialize
    tests/lvmcore/test_lvmcore_serialize.c
  )
  target_link_libraries(test_lvmcore_serialize PRIVATE lvmcore)
  if(TARGET corpus)
    add_dependencies(test_lvmcore_serialize corpus)
  endif()
  add_test(NAME test_lvmcore_serialize
    COMMAND test_lvmcore_serialize "${CORPUS_OUT}")

  # W2 Task 8: multi-VG registry + PV arrival state machine host core.
  # Pure logic test on synthetic VGs — no corpus dependency.
  add_executable(test_winlvm_vg
    tests/lvmcore/test_winlvm_vg.c
    src/driver/winlvm_vg.c
  )
  target_include_directories(test_winlvm_vg PRIVATE src/driver)
  target_link_libraries(test_winlvm_vg PRIVATE lvmcore)
  add_test(NAME test_winlvm_vg COMMAND test_winlvm_vg)

  add_executable(lvmcore_dump_vgcfg
    tests/lvmcore/lvmcore_dump_vgcfg.c
  )
  target_link_libraries(lvmcore_dump_vgcfg PRIVATE lvmcore)
  if(TARGET corpus)
    add_dependencies(lvmcore_dump_vgcfg corpus)
  endif()
endif()
enable_testing()

add_library(winlvm_service_protocol STATIC
  src/service/protocol.cpp
  src/service/mutation.cpp
)
target_include_directories(winlvm_service_protocol PUBLIC src)

add_executable(winlvm_svc src/service/main.cpp)
target_link_libraries(winlvm_svc PRIVATE winlvm_service_protocol)

add_executable(winlvm_tests
  tests/test_main.cpp
  tests/test_units.cpp
  tests/test_corpus.cpp
  tests/test_service.cpp
  tests/test_lvm_cli.cpp
  tests/test_mount_ioctl.cpp
)
target_include_directories(winlvm_tests PRIVATE src)
target_link_libraries(winlvm_tests PRIVATE lvmfmt winlvm_service_protocol)
target_compile_definitions(winlvm_tests PRIVATE
  WINLVM_CORPUS_EXPECTED="${CORPUS_SRC}/expected"
  WINLVM_CORPUS_IMAGES="${CORPUS_OUT}"
  WINLVM_LVM_EXE="$<TARGET_FILE:lvm>"
)
add_dependencies(winlvm_tests lvm)
if(TARGET corpus)
  add_dependencies(winlvm_tests corpus)
endif()

add_test(NAME winlvm_tests COMMAND winlvm_tests)
