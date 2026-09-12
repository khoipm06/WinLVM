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
  # Pass the raw corpus images directory
  add_test(NAME winlvm_driver_parser_tests COMMAND winlvm_driver_parser_tests "${CORPUS_OUT}")

  # Adversarial vectors for the freestanding parser: stripe-array boundary,
  # CRC edge cases, wraparound sizes, fail-closed typeless segments. Fully
  # synthetic — no corpus dependency.
  add_executable(winlvm_lvm_hardening_tests
    tests/lvm_read/test_lvm_read_hardening.c
    src/driver/lvm_read.c
  )
  target_include_directories(winlvm_lvm_hardening_tests PRIVATE src/driver)
  add_test(NAME winlvm_lvm_hardening_tests COMMAND winlvm_lvm_hardening_tests)

  add_executable(winlvm_luks_tests
    tests/luks_read/test_luks_read.c
    src/luksfmt/luks_read.c
  )
  target_include_directories(winlvm_luks_tests PRIVATE src/luksfmt)
  if(TARGET corpus)
    add_dependencies(winlvm_luks_tests corpus)
  endif()
  add_test(NAME winlvm_luks_tests COMMAND winlvm_luks_tests "${CORPUS_OUT}/luks2-default.img")

  # LUKS2 full-chain unlock test (Stage 4): PBKDF2 keyslot + AF merge +
  # digest verify + XTS sector decrypt against the pbkdf2 corpus image.
  add_executable(winlvm_luks_unlock_tests
    tests/luks_read/test_luks_unlock.c
    src/luksfmt/luks_read.c
    src/luksfmt/luks_unlock.c
    src/crypto/wl_sha256.c
    src/crypto/wl_aes_xts.c
  )
  target_include_directories(winlvm_luks_unlock_tests PRIVATE src/luksfmt src/crypto)
  if(TARGET corpus)
    add_dependencies(winlvm_luks_unlock_tests corpus)
  endif()
  add_test(NAME winlvm_luks_unlock_tests COMMAND winlvm_luks_unlock_tests "${CORPUS_OUT}/luks2-pbkdf2.img")

  # LUKS+LVM integration: unlock container, parse PV, hash LV vs oracle.
  add_executable(winlvm_luks_lvm_chain_tests
    tests/luks_read/test_luks_lvm_chain.c
    src/luksfmt/luks_read.c
    src/luksfmt/luks_unlock.c
    src/driver/lvm_read.c
    src/crypto/wl_sha256.c
    src/crypto/wl_aes_xts.c
  )
  target_include_directories(winlvm_luks_lvm_chain_tests PRIVATE
    src/luksfmt src/crypto src/driver)
  if(TARGET corpus)
    add_dependencies(winlvm_luks_lvm_chain_tests corpus)
  endif()
  add_test(NAME winlvm_luks_lvm_chain_tests
    COMMAND winlvm_luks_lvm_chain_tests
            "${CORPUS_OUT}/luks2-pbkdf2.img"
            "${CORPUS_SRC}/expected")

  # Crypto known-answer tests (SHA-256/HMAC/PBKDF2/AES/XTS).
  add_executable(winlvm_crypto_tests
    tests/crypto/test_crypto.c
    src/crypto/wl_sha256.c
    src/crypto/wl_aes_xts.c
  )
  target_include_directories(winlvm_crypto_tests PRIVATE src/crypto)
  add_test(NAME winlvm_crypto_tests COMMAND winlvm_crypto_tests)
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
