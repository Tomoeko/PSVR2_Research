if(NOT DEFINED FIXTURE_ROOT)
    message(FATAL_ERROR "FIXTURE_ROOT is required")
endif()

set(KERNEL
    "${FIXTURE_ROOT}/01.10/kernel.bin")
set(SIEUSB
    "${FIXTURE_ROOT}/01.10/sieusb.ko")

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

require_sha256("${KERNEL}"
    "a6d3cfcbf7e1f490aa043464c14230e927ad9c4ca54d81df8f927ee48ce88773")
require_sha256("${SIEUSB}"
    "ec88601472492fd97a5752c103b211236a83e7dd9b74f26ffe73dc51bb395f34")

# Raw kernel offsets are virtual address - 0xffffffc000080000.
require_hex("${KERNEL}" 601000 32
    "9f3a03d51f8308d59f3b03d5df3f03d5f35341a9f51340f9fd7bc3a8c0035fd6"
    "01.10 TLBI VMALLE1IS gadget")
require_hex("${KERNEL}" 2639452 24
    "e00315aacaa3019496360339f35341a9f55b42a9fd7bc3a8"
    "01.10 mtu3 completion resume and epilogue")
require_hex("${KERNEL}" 2636860 24
    "f35341a9f55b42a9f76343a9f92340f9fd7bc6a8c0035fd6"
    "01.10 mtu3 ep0 ISR epilogue")
require_hex("${KERNEL}" 2643776 24
    "fd7bbfa9fd030091221840f940003fd6fd7bc1a8c0035fd6"
    "01.10 gadget-core clean return")
require_hex("${KERNEL}" 3069320 32
    "e103009122c47292411840b921040011411800b9110080f901fc5f8822404011"
    "01.10 raw spin-lock entry")
require_hex("${KERNEL}" 34032 32
    "fd7bbea91f0440f2fd030091f35301a981010054f30300aa3fffff97f403002a"
    "01.10 aarch64 instruction patch helper")
require_hex("${KERNEL}" 33888 12
    "f35341a9fd7bc3a8c0035fd6"
    "01.10 cold-bootstrap frame epilogue")
require_hex("${KERNEL}" 34224 80
    "620640a9807e409394060011217860b8407860f8cbffff972001003463520091710080f9617c5f8821040011617c0288a2ffff35edffff1714008052601240b91f00146becfdff5400008052f4ffff17"
    "01.10 cold-bootstrap instruction patch loop")
require_zero("${KERNEL}" 3075568 528
    "01.10 verified text patch cave")
require_hex("${KERNEL}" 6197120 128
    "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
    "01.10 kernel probe padding")

message(STATUS "Exact 01.10 firmware binary regression checks passed")
