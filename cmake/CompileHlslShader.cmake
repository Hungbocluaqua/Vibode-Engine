if(NOT DEFINED DXC OR DXC STREQUAL "")
    message(FATAL_ERROR "DXC is required")
endif()
if(NOT DEFINED SOURCE OR SOURCE STREQUAL "")
    message(FATAL_ERROR "SOURCE is required")
endif()
if(NOT DEFINED OUTPUT OR OUTPUT STREQUAL "")
    message(FATAL_ERROR "OUTPUT is required")
endif()
if(NOT DEFINED INCLUDE_DIR OR INCLUDE_DIR STREQUAL "")
    message(FATAL_ERROR "INCLUDE_DIR is required")
endif()
if(NOT DEFINED PROFILE OR PROFILE STREQUAL "")
    set(PROFILE "cs_6_6")
endif()
if(NOT DEFINED ENTRY_POINT OR ENTRY_POINT STREQUAL "")
    set(ENTRY_POINT "main")
endif()

file(MAKE_DIRECTORY "${OUTPUT_DIR}")

execute_process(
    COMMAND "${DXC}"
        -spirv
        -fspv-target-env=vulkan1.3
        -fspv-reflect
        -HV 2021
        -T "${PROFILE}"
        -E "${ENTRY_POINT}"
        -D SPIRV=1
        -I "${INCLUDE_DIR}"
        -Fo "${OUTPUT}"
        "${SOURCE}"
    RESULT_VARIABLE compile_result)

if(NOT compile_result EQUAL 0)
    message(FATAL_ERROR "DXC failed for ${SOURCE}")
endif()

file(WRITE "${OUTPUT}.options"
    "PROFILE=${PROFILE}\nENTRY_POINT=${ENTRY_POINT}\nSPIRV=1\nINCLUDE_DIR=${INCLUDE_DIR}\n")
