# DingusPPC-Pippin modifications
# Copyright (c) 2026 Iain Bennett
# SPDX-License-Identifier: GPL-3.0-or-later
# Change log:
#   2026-11-15 - Q219 baseline (Q48 to Q219, before version control): build the ARM64 JIT and fastmem sources.
# Full history: PIPPIN.md and Git history.

include_directories("${PROJECT_SOURCE_DIR}"
                    "${PROJECT_SOURCE_DIR}/thirdparty/loguru/")

file(GLOB SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
                  "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
    )

# The use of volatile is deprecated, but we still need it to avoid function
# parameters being clobbered when using setjmp/longjmp.
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(-Wno-deprecated-volatile)
endif()

add_library(cpu_ppc OBJECT ${SOURCES})

# Private Pippin hybrid only. Upstream/default regression arena remains 16 MiB.
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/nw_jit.cpp" PROPERTIES COMPILE_DEFINITIONS "NW_JIT_CODE_BITS=27")

target_sources(cpu_ppc PRIVATE pippin_fastmem.cpp)
