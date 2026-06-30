if(NOT DEFINED GLSLANG_VALIDATOR OR GLSLANG_VALIDATOR STREQUAL "")
    message(FATAL_ERROR "GLSLANG_VALIDATOR is required")
endif()
if(NOT DEFINED SOURCE OR SOURCE STREQUAL "")
    message(FATAL_ERROR "SOURCE is required")
endif()
if(NOT DEFINED OUTPUT OR OUTPUT STREQUAL "")
    message(FATAL_ERROR "OUTPUT is required")
endif()
if(NOT DEFINED DEPFILE OR DEPFILE STREQUAL "")
    message(FATAL_ERROR "DEPFILE is required")
endif()

if(NOT DEFINED RTV_USE_DIMENSIONED_SAMPLER OR RTV_USE_DIMENSIONED_SAMPLER STREQUAL "")
    set(RTV_USE_DIMENSIONED_SAMPLER "1")
endif()
if(NOT DEFINED RTV_DENOISER_SHARED_TILE OR RTV_DENOISER_SHARED_TILE STREQUAL "")
    set(RTV_DENOISER_SHARED_TILE "1")
endif()
if(NOT DEFINED RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT OR RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT STREQUAL "")
    set(RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT "0")
endif()
if(NOT DEFINED EXTRA_DEFINES)
    set(EXTRA_DEFINES "")
endif()

file(MAKE_DIRECTORY "${OUTPUT_DIR}")

set(extra_defines)
if(NOT EXTRA_DEFINES STREQUAL "")
    string(REPLACE "|" ";" extra_defines "${EXTRA_DEFINES}")
endif()

set(define_args
    "-DRTV_USE_DIMENSIONED_SAMPLER=${RTV_USE_DIMENSIONED_SAMPLER}"
    "-DRTV_DENOISER_SHARED_TILE=${RTV_DENOISER_SHARED_TILE}")

set(has_restir_gi_layout_define OFF)
foreach(define IN LISTS extra_defines)
    if(define MATCHES "^RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT=")
        set(has_restir_gi_layout_define ON)
    endif()
endforeach()

if(NOT has_restir_gi_layout_define)
    list(APPEND define_args "-DRTV_RESTIR_GI_UNCOMPRESSED_LAYOUT=${RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT}")
endif()

foreach(define IN LISTS extra_defines)
    list(APPEND define_args "-D${define}")
endforeach()

execute_process(
    COMMAND "${GLSLANG_VALIDATOR}"
        -V --target-env vulkan1.3
        --depfile "${DEPFILE}"
        ${define_args}
        -o "${OUTPUT}"
        "${SOURCE}"
    RESULT_VARIABLE compile_result)

if(NOT compile_result EQUAL 0)
    message(FATAL_ERROR "glslangValidator failed for ${SOURCE}")
endif()

set(signature "RTV_USE_DIMENSIONED_SAMPLER=${RTV_USE_DIMENSIONED_SAMPLER}\n")
string(APPEND signature "RTV_DENOISER_SHARED_TILE=${RTV_DENOISER_SHARED_TILE}\n")
if(NOT has_restir_gi_layout_define)
    string(APPEND signature "RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT=${RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT}\n")
endif()
foreach(define IN LISTS extra_defines)
    string(APPEND signature "${define}\n")
endforeach()
file(WRITE "${OUTPUT}.options" "${signature}")
