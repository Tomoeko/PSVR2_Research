if(NOT DEFINED FIXTURE_ROOT)
    message(FATAL_ERROR "FIXTURE_ROOT is required")
endif()

set(ELF "${FIXTURE_ROOT}/06.00/kernel.elf")
set(BIN "${FIXTURE_ROOT}/06.00/kernel.bin")
set(DUMP1 "${FIXTURE_ROOT}/06.00/kernel-dump-a.bin")
set(DUMP2 "${FIXTURE_ROOT}/06.00/kernel-dump-b.bin")
set(SIEUSB "${FIXTURE_ROOT}/06.00/sieusb.ko")
set(LIBCOMPOSITE
    "${FIXTURE_ROOT}/06.00/libcomposite.ko")

function(require_sha256 path expected)
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "Missing exact binary: ${path}")
    endif()
    file(SHA256 "${path}" actual)
    if(NOT actual STREQUAL expected)
        message(FATAL_ERROR
            "Exact binary hash mismatch for ${path}\n"
            "expected=${expected}\nactual=${actual}")
    endif()
endfunction()

function(require_hex path offset length expected label)
    file(READ "${path}" actual OFFSET ${offset} LIMIT ${length} HEX)
    string(TOLOWER "${actual}" actual)
    if(NOT actual STREQUAL expected)
        message(FATAL_ERROR
            "${label} mismatch in ${path} at offset ${offset}\n"
            "expected=${expected}\nactual=${actual}")
    endif()
endfunction()

function(require_zero path offset length label)
    file(READ "${path}" actual OFFSET ${offset} LIMIT ${length} HEX)
    string(REGEX REPLACE "0" "" nonzero "${actual}")
    if(NOT nonzero STREQUAL "")
        message(FATAL_ERROR
            "${label} is not zero-filled in ${path} at offset ${offset}")
    endif()
endfunction()

require_sha256("${ELF}"
    "df247730d229947ca21aaf4c614764af99e65e322ba2afaf243559614b7c665c")
require_sha256("${BIN}"
    "5eea9d6da46efc8230183baf70023bef675f8dd09145d575b48f88a6337ca347")
require_sha256("${DUMP1}"
    "a2f6bfc3b4938c4e55f52a796f97ef10158c7e695f530869acb85b3d31fe2ef1")
require_sha256("${DUMP2}"
    "567e58ebe3a67471a205f9394aa6e404d492b677fb1cd65626f21ea6606d62d4")
require_sha256("${SIEUSB}"
    "75540817d557e812381663bd060d7f22f79394b2e4555693d8d05e92d6594f7e")
require_sha256("${LIBCOMPOSITE}"
    "e88291249b1a9cbe1c7098df6befab096bbcac5a34105f6b09a1c6f001e64355")

find_program(NM_TOOL NAMES llvm-nm nm REQUIRED)
execute_process(
    COMMAND "${NM_TOOL}" -n "${ELF}"
    RESULT_VARIABLE NM_RESULT
    OUTPUT_VARIABLE NM_SYMBOLS
    ERROR_VARIABLE NM_ERROR
)
if(NOT NM_RESULT EQUAL 0)
    message(FATAL_ERROR "Could not inspect exact ELF symbols: ${NM_ERROR}")
endif()

function(require_symbol address name)
    string(REGEX MATCH
        "(^|\n)${address}[ \t]+[A-Za-z][ \t]+${name}(\n|$)"
        match "${NM_SYMBOLS}")
    if(NOT match)
        message(FATAL_ERROR
            "Expected exact ELF symbol ${name} at ${address}")
    endif()
endfunction()

require_symbol("ffffffc000668f6c" "__bss_stop")
require_symbol("ffffffc000669000" "idmap_pg_dir")
require_symbol("ffffffc00066c000" "swapper_pg_dir")
require_symbol("ffffffc00066e000" "_end")
require_symbol("ffffffc000660238" "gsCwq")
require_symbol("ffffffc000660730" "gsMMUFaultData")
require_symbol("ffffffc0000884f0" "aarch64_insn_patch_text_nosync")
require_symbol("ffffffc00011bad0" "__kmalloc")
require_symbol("ffffffc00011ccc8" "kfree")
require_symbol("ffffffc00036edd0" "crval")
require_symbol("ffffffc00036f000" "__start_rodata")
require_symbol("ffffffc000303658" "mtu3_ep0_isr")
require_symbol("ffffffc0003045fc" "mtu3_req_complete")
require_symbol("ffffffc000305768" "usb_gadget_giveback_request")
require_symbol("ffffffc00036d5b8" "_raw_spin_lock")

# Exact 06.00 raw kernel offsets (VA - 0xffffffc000080000).
require_hex("${BIN}" 601076 32
    "9f3a03d51f8308d59f3b03d5df3f03d5f35341a9f51340f9fd7bc3a8c0035fd6"
    "TLBI VMALLE1IS gadget")
require_hex("${BIN}" 2643816 24
    "fd7bbfa9fd030091221840f940003fd6fd7bc1a8c0035fd6"
    "usb_gadget_giveback_request clean return")
require_zero("${BIN}" 3075568 528
    "verified .text patch cave")
require_hex("${BIN}" 34032 32
    "fd7bbea91f0440f2fd030091f35301a981010054f30300aa3fffff97f403002a"
    "aarch64 instruction patch helper")
require_hex("${BIN}" 33888 12
    "f35341a9fd7bc3a8c0035fd6"
    "cold bootstrap x19/x20 frame epilogue")
require_hex("${BIN}" 34224 80
    "620640a9807e409394060011217860b8407860f8cbffff972001003463520091710080f9617c5f8821040011617c0288a2ffff35edffff1714008052601240b91f00146becfdff5400008052f4ffff17"
    "cold bootstrap exact instruction patch loop")
require_hex("${BIN}" 601092 24
    "f35341a9f51340f9fd7bc3a8c0035fd660fe4cd394fe4cd3"
    "x19/x20/x21 ROP loader")
require_hex("${BIN}" 10684 44
    "e103152ae00314aa60023fd660fcff34f0ffff17fd7bbfa900a080520003a072fd030091c71c0094000c4093"
    "two-argument call and zero-return epilogue gadget")
require_hex("${BIN}" 2639492 24
    "e00315aacca3019496360339f35341a9f55b42a9fd7bc3a8"
    "mtu3 completion relock, busy restore, and epilogue")
require_hex("${BIN}" 2636900 24
    "f35341a9f55b42a9f76343a9f92340f9fd7bc6a8c0035fd6"
    "mtu3 ep0 ISR epilogue")
require_hex("${BIN}" 2639368 16
    "f30301aaf55b02a9f40300aa16344339"
    "mtu3 completion live request, endpoint, and busy registers")
require_hex("${BIN}" 2639436 16
    "753a40f92000805280360339e00315aa"
    "mtu3 request controller pointer and endpoint unlock")
require_hex("${BIN}" 2640400 8
    "013c40f97dfdff97"
    "mtu3 controller ep0 pointer at offset 0x78")
require_zero("${BIN}" 3075968 52
    "permanent patch and TLBI cleanup helpers")
require_hex("${BIN}" 637648 32
    "fd7bbba91f0840f1fd030091f35301a9f55b02a9f76303a9f92300f909020054"
    "__kmalloc entry")

# sieusb.ko .text begins at file offset 0x64.  These exact instructions prove
# both overflow routes and, critically, the mode-2 copy into SP+0x38.  A
# The 120-byte copy begins at SP+0x38 and overwrites exactly the caller's
# 0x30-byte frame consumed by the TLBI gadget, leaving mtu3_ep0_isr untouched.
require_hex("${SIEUSB}" 1140 32
    "fd7bb8a9fd030091f35301a9f30301aaf55b02a91400009081020091bfff03a9"
    "sieusb sub_410 0x80-byte frame")
require_hex("${SIEUSB}" 1612 16
    "f35341a9f55b42a9fd7bc8a8c0035fd6"
    "sieusb sub_410 epilogue")
require_hex("${SIEUSB}" 1628 32
    "fd7bb8a9e20300aae00301aafd030091f35301a914000090f55b02a981020091"
    "sieusb sub_5F8 0x80-byte frame")
require_hex("${SIEUSB}" 1752 52
    "020840b975a20391bfff03a9a0e30091bfff04a9427c4093bfff05a9bfff06a900000094e00315aa00000094f60300aa64be4339"
    "sieusb mode-2 SP+0x38 copy and lock state")
require_hex("${SIEUSB}" 1952 16
    "f35341a9f55b42a9fd7bc8a8c0035fd6"
    "sieusb sub_5F8 epilogue")

# The live request callback resolves to libcomposite.ko+0x129c.  Pin its
# exact leaf implementation so callback discovery cannot silently validate
# against a different module image.
require_hex("${LIBCOMPOSITE}" 4864 32
    "201c40f9c00200b4030440f93f0003eba10000540150423921781f1201500239"
    "libcomposite live request callback")

# 0xffffffc000668f80 is linker padding between __bss_stop and idmap_pg_dir.
# It is stable in both exact live captures and has no exact-image xrefs.
require_hex("${DUMP1}" 6197120 128
    "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
    "kernel probe padding capture 1")
require_hex("${DUMP2}" 6197120 128
    "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
    "kernel probe padding capture 2")

message(STATUS "Exact 06.00 firmware binary regression checks passed")
