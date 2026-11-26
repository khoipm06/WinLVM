cmake_minimum_required(VERSION 3.24)
project(winlvm LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8 /Zc:__cplusplus)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
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
  src/lvmcore/lvmcore_thin.c
  src/lvmcore/lvmcore_snap.c
  src/lvmcore/lvmcore_raid.c
)
set_target_properties(lvmcore PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
)
target_include_directories(lvmcore PUBLIC src/lvmcore/include)
if(NOT MSVC)
  target_compile_options(lvmcore PRIVATE -Wframe-larger-than=512)
  set_source_files_properties(
    src/driver/winlvm_vg.c
    src/driver/winlvm_map.c
    src/driver/winlvm_thin.c
    src/driver/winlvm_snap.c
    src/driver/winlvm_raid.c
    PROPERTIES COMPILE_OPTIONS "-Wframe-larger-than=512"
  )
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
  src/lvmfmt/allocator.cpp
  src/lvmfmt/commit.cpp
)
target_include_directories(lvmfmt PUBLIC src/lvmfmt/include)

# ---------------------------------------------------------------------------
# lvmdump — inspect an image and print its computed device-mapper table.
# ---------------------------------------------------------------------------
add_executable(lvmdump src/tools/lvmdump/main.cpp)
target_link_libraries(lvmdump PRIVATE lvmfmt)

# lvm.exe — comprehensive LVM2 CLI control plane.
add_executable(lvm
  src/tools/lvm/main.cpp
  src/tools/lvm/cmd_pv.cpp
  src/tools/lvm/cmd_vg.cpp
  src/tools/lvm/cmd_lv_create.cpp
  src/tools/lvm/cmd_lv_resize.cpp
  src/tools/lvm/cmd_lv_manage.cpp
  src/tools/lvm/lv_extent_alloc.cpp
  src/tools/lvm/cmd_misc.cpp
)
target_include_directories(lvm PRIVATE src/tools/lvm)
target_link_libraries(lvm PRIVATE lvmfmt)

# ---------------------------------------------------------------------------
# Test Corpus. Disk images are generated deterministically out-of-tree at build
# time by scripts/gen_test_fixtures.py; only lightweight text/hash oracles live
# under corpus/expected/.
# ---------------------------------------------------------------------------
set(CORPUS_SRC "${CMAKE_CURRENT_SOURCE_DIR}/corpus")
set(CORPUS_OUT "${CMAKE_BINARY_DIR}/corpus-images" CACHE PATH "Directory containing generated corpus images")

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(CORPUS_IMAGE_NAMES
  linear-1pv.img
  linear-1pv-multiseg.img
  linear-2pv-a.img
  linear-2pv-b.img
  striped-2pv-a.img
  striped-2pv-b.img
  sector4k.img
  odd-geometry.img
  thin-pool.img
  snapshot.img
  raid1-mirror-a.img
  raid1-mirror-b.img
  mixed-vg-a.img
  mixed-vg-b.img
  luks2-default.img
  luks2-pbkdf2.img
)
set(CORPUS_IMAGES "")
foreach(iname ${CORPUS_IMAGE_NAMES})
  list(APPEND CORPUS_IMAGES "${CORPUS_OUT}/${iname}")
endforeach()

set(FIXTURE_GEN_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/scripts/gen_test_fixtures.py")
add_custom_command(
  OUTPUT ${CORPUS_IMAGES}
  COMMAND "${Python3_EXECUTABLE}" "${FIXTURE_GEN_SCRIPT}" --out-dir "${CORPUS_OUT}"
  DEPENDS "${FIXTURE_GEN_SCRIPT}"
  COMMENT "Generating deterministic LVM2/LUKS2 test fixtures into ${CORPUS_OUT}"
  VERBATIM
)
add_custom_target(corpus ALL DEPENDS ${CORPUS_IMAGES})

enable_testing()

# Synthetic freestanding C11 unit tests (no corpus images required).
add_executable(test_lvmcore_hardening
  tests/lvmcore/test_lvmcore_hardening.c
)
target_link_libraries(test_lvmcore_hardening PRIVATE lvmcore)
add_test(NAME test_lvmcore_hardening COMMAND test_lvmcore_hardening)

# Host logic tests for multi-VG registry, PV arrival state machine, mapper, and block engines.
add_executable(test_winlvm_vg
  tests/lvmcore/test_winlvm_vg.c
  src/driver/winlvm_vg.c
  src/driver/winlvm_map.c
  src/driver/winlvm_thin.c
  src/driver/winlvm_snap.c
  src/driver/winlvm_raid.c
)
target_include_directories(test_winlvm_vg PRIVATE src/driver)
target_link_libraries(test_winlvm_vg PRIVATE lvmcore)
add_test(NAME test_winlvm_vg COMMAND test_winlvm_vg)

# ---------------------------------------------------------------------------
# lvmcore corpus-backed parser, serialization, and differential mapper tests.
# ---------------------------------------------------------------------------
add_executable(test_lvmcore_parser
  tests/lvmcore/test_lvmcore_parser.c
)
target_link_libraries(test_lvmcore_parser PRIVATE lvmcore)
add_dependencies(test_lvmcore_parser corpus)
add_test(NAME test_lvmcore_parser
  COMMAND test_lvmcore_parser "${CORPUS_OUT}" "${CORPUS_SRC}/expected")

add_executable(test_lvmcore_serialize
  tests/lvmcore/test_lvmcore_serialize.c
)
target_link_libraries(test_lvmcore_serialize PRIVATE lvmcore)
add_dependencies(test_lvmcore_serialize corpus)
add_test(NAME test_lvmcore_serialize
  COMMAND test_lvmcore_serialize "${CORPUS_OUT}")

# Differential kernel-mapper suite against generated corpus images and SHA-256 oracles.
add_executable(test_driver_corpus_map
  tests/lvmcore/test_driver_corpus_map.c
  tests/test_sha256.c
  src/driver/winlvm_vg.c
  src/driver/winlvm_map.c
  src/driver/winlvm_thin.c
  src/driver/winlvm_snap.c
  src/driver/winlvm_raid.c
)
target_include_directories(test_driver_corpus_map PRIVATE src/driver tests)
target_link_libraries(test_driver_corpus_map PRIVATE lvmcore)
add_dependencies(test_driver_corpus_map corpus)
add_test(NAME test_driver_corpus_map
  COMMAND test_driver_corpus_map "${CORPUS_OUT}" "${CORPUS_SRC}/expected")

add_executable(lvmcore_dump_vgcfg
  tests/lvmcore/lvmcore_dump_vgcfg.c
)
target_link_libraries(lvmcore_dump_vgcfg PRIVATE lvmcore)
add_dependencies(lvmcore_dump_vgcfg corpus)

add_library(winlvm_service_protocol STATIC
  src/service/protocol.cpp
  src/service/mutation.cpp
)
target_include_directories(winlvm_service_protocol PUBLIC src)

add_executable(winlvm_svc
  src/service/main.cpp
  src/service/backend_win32.cpp
)
target_link_libraries(winlvm_svc PRIVATE winlvm_service_protocol lvmfmt)
if(WIN32)
  target_link_libraries(winlvm_svc PRIVATE advapi32 user32)
endif()

add_executable(winlvm_tests
  tests/test_main.cpp
  tests/test_units.cpp
  tests/test_corpus.cpp
  tests/test_service.cpp
  tests/test_lvm_cli.cpp
  tests/test_storage_surface.cpp
  tests/test_lvm_control_plane.cpp
  tests/test_sha256.c
  src/tools/lvm/lv_extent_alloc.cpp
  src/driver/winlvm_storage.c
  src/driver/winlvm_mount.c
)
target_include_directories(winlvm_tests PRIVATE src tests src/driver)
target_link_libraries(winlvm_tests PRIVATE lvmfmt winlvm_service_protocol lvmcore)
target_compile_definitions(winlvm_tests PRIVATE
  WINLVM_CORPUS_EXPECTED="${CORPUS_SRC}/expected"
  WINLVM_CORPUS_IMAGES="${CORPUS_OUT}"
  WINLVM_LVM_EXE="$<TARGET_FILE:lvm>"
  WINLVM_LVMDUMP_EXE="$<TARGET_FILE:lvmdump>"
)
add_dependencies(winlvm_tests lvm lvmdump)
if(TARGET corpus)
  add_dependencies(winlvm_tests corpus)
endif()

add_test(NAME winlvm_tests COMMAND winlvm_tests)
