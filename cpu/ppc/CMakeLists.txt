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
