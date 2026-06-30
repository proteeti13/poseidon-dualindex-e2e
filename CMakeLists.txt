cmake_minimum_required(VERSION 3.14)
project(poseidon_core)

if(${CMAKE_PROJECT_NAME} STREQUAL ${PROJECT_NAME})
#
# Set this only if this project is built as a standalone project.
#
#-------------------------------------------------------------------------------
# Set the mount path of your pmem device where the tests/benchmarks are stored
if(NOT PMEM_MNT_PATH)
  set(PMEM_MNT_PATH "/mnt/pmem0/poseidon")
endif()

option(USE_PMDK         "Use Intel PMDK"                         OFF)
option(USE_LLVM         "Use LLVM for query compilation"         OFF)
option(USE_IN_MEMORY    "Use only in-memory storage"             OFF)
option(BUILD_PYTHON     "Build python interface for poseidon"    ON)
option(QOP_PROFILING    "Enable query profiling"                 ON)
option(USE_GUNROCK      "Use Gunrock for graph analytics"        OFF)
option(CSR_DELTA        "Use delta store for updating CSR"       OFF)
option(USE_UPMEM        "Use UPMEM SDK for PIM"                  OFF)
#-------------------------------------------------------------------------------
endif()

if(${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
  set(USE_PMDK OFF)
  set(LLVM_DIR "/opt/homebrew/opt/llvm@16/lib/cmake/llvm")
  set(ANTLR_EXECUTABLE "/opt/homebrew/Cellar/antlr/4.13.1/antlr-4.13.1-complete.jar")
endif()

# Don't try to contact github everytime.
set (FETCHCONTENT_FULLY_DISCONNECTED "OFF")

# Set the searching location for cmake 'include' locations
set(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# Include the sanitizer module
include(sanitizers)

set(USE_SANITIZER "Memory")
set(CMAKE_MACOSX_RPATH 1)

# Include the code coverage module
# option(CODE_COVERAGE ON)
# include(code-coverage)
# add_code_coverage()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent REQUIRED)

#-------------------------------------------------------------------------------
# ANTLR4 Parser generator
#

find_package(ANTLR)

if(ANTLR_FOUND)
# add macros to generate ANTLR Cpp code from grammar
message("ANTLR found")
  antlr_target(grammar src/query/parser/poseidon.g4 
    COMPILE_FLAGS -Xexact-output-dir -visitor
    OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/src/query/antlr4_generated
  )
else()
  set(ANTLR_grammar_CXX_OUTPUTS 
    src/query/antlr4_generated/poseidonLexer.h
    src/query/antlr4_generated/poseidonLexer.cpp
    src/query/antlr4_generated/poseidonParser.h
    src/query/antlr4_generated/poseidonParser.cpp)
endif()

# message("grammar: ${ANTLR_grammar_CXX_OUTPUTS}")

#-------------------------------------------------------------------------------
# Storage system: PMEM, IN_MEMORY or PFILES

if(USE_PMDK MATCHES ON AND USE_IN_MEMORY MATCHES ON)
  message(FATAL_ERROR "Cannot use PMDK and IN_MEMORY at the same time.")
endif()

if(USE_PMDK MATCHES ON AND USE_IN_MEMORY MATCHES OFF)
  message(STATUS "looking for PMDK")
  add_definitions( "-DUSE_PMDK" )
# Include the PMDK module
include(pmdk)
endif()


if(USE_IN_MEMORY MATCHES ON)
  add_definitions( "-DUSE_IN_MEMORY" )
endif()

if(USE_PMDK MATCHES OFF AND USE_IN_MEMORY MATCHES OFF)
  add_definitions( "-DUSE_PFILES" )
endif()

#-------------------------------------------------------------------------------

if(USE_UPMEM MATCHES ON)
  add_definitions( "-DUSE_UPMEM" )
  set(USE_IN_MEMORY ON)
  set(USE_PMDK OFF)
  include_directories(/usr/include/dpu)
  add_library(dpu SHARED IMPORTED)
  set(DPU_LIB "-ldpu")
endif()

if(QOP_PROFILING MATCHES ON)
  add_definitions( "-DQOP_PROFILING" )
endif()

if(USE_LLVM MATCHES ON)
  add_definitions( "-DUSE_LLVM" )
endif()


if(CSR_DELTA MATCHES ON)
  add_definitions( "-DCSR_DELTA" )
endif()

if (USE_GUNROCK MATCHES ON)
  set(CUDA_MIN_VERSION 9.0)
  FIND_PACKAGE(CUDA ${CUDA_MIN_VERSION} REQUIRED)
  if(NOT DEFINED CMAKE_CUDA_STANDARD)
    set(CMAKE_CUDA_STANDARD 17)
    set(CMAKE_CUDA_STANDARD_REQUIRED ON)
  endif()

  SET(CMAKE_CUDA_ARCHITECTURES 70)
  SET(CUDA_SEPARABLE_COMPILATION ON)
  set(CUDA_PROPAGATE_HOST_FLAGS ON)

  add_definitions( "-DUSE_GUNROCK" )
endif()

configure_file(
  ${PROJECT_SOURCE_DIR}/src/config.h.in
  ${PROJECT_BINARY_DIR}/generated/config.h
)

#---------------------------------------------------------------------------

include_directories(${PROJECT_BINARY_DIR}/generated)
include_directories(${PROJECT_SOURCE_DIR}/src/vec)
include_directories(${PROJECT_SOURCE_DIR}/src/storage)
include_directories(${PROJECT_SOURCE_DIR}/src/bp_file)
include_directories(${PROJECT_SOURCE_DIR}/src/query)
include_directories(${PROJECT_SOURCE_DIR}/src/query/parser)
include_directories(${PROJECT_SOURCE_DIR}/src/query/codegen)
include_directories(${PROJECT_SOURCE_DIR}/src/query/utils)
include_directories(${PROJECT_SOURCE_DIR}/src/query/plan_op)
include_directories(${PROJECT_SOURCE_DIR}/src/query/interp)
include_directories(${PROJECT_SOURCE_DIR}/src/query/expr)
include_directories(${PROJECT_SOURCE_DIR}/src/query/compile)
include_directories(${PROJECT_SOURCE_DIR}/src/tx)
include_directories(${PROJECT_SOURCE_DIR}/src/dict)
include_directories(${PROJECT_SOURCE_DIR}/src/btree)
include_directories(/home/proteeti/DualIndex/indexes)
# has router.hpp, base_index.hpp and learned/ subfolder (zmindex.hpp is inside learned/)
include_directories(/home/proteeti/DualIndex/indexes/pgm)
#has pgm math library pgm_index.hpp, morton_nd.hpp. zmindex and flood internally includes pgm.
include_directories(/home/proteeti/DualIndex/utils)
#includes the basic data types like type.hpp, common.hpp -> point_t, box_t (both zmindex and flood uses this)
include_directories(${PROJECT_SOURCE_DIR}/src/pool)
include_directories(${PROJECT_SOURCE_DIR}/src/log)
include_directories(${PROJECT_SOURCE_DIR}/src/analytics)
include_directories(${PROJECT_SOURCE_DIR}/src/analytics/gunrock)
include_directories(${PROJECT_SOURCE_DIR}/src/analytics/util)
include_directories(${PROJECT_SOURCE_DIR}/src/upmem/aggregation/include)
include_directories(${PROJECT_SOURCE_DIR}/src/antlr4_cpp_runtime/src)
include_directories(${PROJECT_BINARY_DIR}/antlr4cpp_generated_src)
if (USE_GUNROCK MATCHES ON)
  include_directories(${CUDA_INCLUDE_DIRS})
endif()

#---------------------------------------------------------------------------

set(THIRD_PARTY_DIR "${PROJECT_BINARY_DIR}/_deps")

# C++ compiler flags
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS} -Wall -Wno-deprecated -O3 -Wsign-compare")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS} -Wall -Wno-deprecated -O0 -g -Wsign-compare")
if ("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-unused-local-typedefs -Wno-#pragma-messages")
elseif("${CMAKE_CXX_COMPILER_ID}" MATCHES "GNU")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-unused-local-typedefs -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -Wno-unused")
elseif("${CMAKE_CXX_COMPILER_ID}" MATCHES "Intel")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -wd488 -wd597")
endif()

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wno-unused -Wno-uninitialized")

if(CMAKE_BUILD_TYPE MATCHES Release)
  # this disables asserts for release builds
  add_definitions("-DNDEBUG")
endif()

if (USE_GUNROCK MATCHES ON)
  string(APPEND CMAKE_CUDA_FLAGS " -Xcompiler -Wall")
endif()

#---------------------------------------------------------------------------

# Gunrock =================================================================== #
# TODO: Throw exception if /usr/lib/libgunrock.so is not found. Right now cmake 
#       will not inform you, but the linker will fail to finish building Poseidon

if(USE_GUNROCK MATCHES ON)
add_library(gunrock SHARED IMPORTED)
set_property(TARGET gunrock PROPERTY IMPORTED_LOCATION "/usr/lib/libgunrock.so")
message(STATUS "Include library gunrock. Make sure /usr/lib/libgunrock.so exists!")
endif()

# Catch2 =================================================================== #
FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG        v3.6.0
)

FetchContent_MakeAvailable(Catch2)

# CSVParser =================================================================== #
 FetchContent_Declare(
  CSV
  GIT_REPOSITORY https://github.com/AriaFallah/csv-parser.git
)

FetchContent_MakeAvailable(CSV)

# spdlog =================================================================== #
 FetchContent_Declare(
  spdlog
  GIT_REPOSITORY https://github.com/gabime/spdlog.git
   GIT_TAG        v1.13.0
)

FetchContent_MakeAvailable(spdlog)

if(USE_LLVM MATCHES ON)
# LLVM ============================================================== #
find_package(LLVM REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")
include_directories(${LLVM_INCLUDE_DIRS})
execute_process(COMMAND llvm-config --libs all
                OUTPUT_VARIABLE LLVM_LIBRARIES)
# llvm_map_components_to_libnames(LLVM_LIBRARIES
# XRay WindowsManifest TableGen Symbolize DebugInfoPDB OrcJIT OrcError JITLink ObjectYAML MCA LTO Passes Coroutines ObjCARCOpts Extensions LineEditor LibDriver Interpreter FuzzMutate MCJIT ExecutionEngine RuntimeDyld DWARFLinker DlltoolDriver Option DebugInfoGSYM Coverage XCoreDisassembler XCoreCodeGen XCoreDesc XCoreInfo X86Disassembler X86AsmParser X86CodeGen X86Desc X86Info WebAssemblyDisassembler WebAssemblyCodeGen WebAssemblyDesc WebAssemblyAsmParser WebAssemblyInfo SystemZDisassembler SystemZCodeGen SystemZAsmParser SystemZDesc SystemZInfo SparcDisassembler SparcCodeGen SparcAsmParser SparcDesc SparcInfo RISCVDisassembler RISCVCodeGen RISCVAsmParser RISCVDesc RISCVUtils RISCVInfo PowerPCDisassembler PowerPCCodeGen PowerPCAsmParser PowerPCDesc PowerPCInfo NVPTXCodeGen NVPTXDesc NVPTXInfo MSP430Disassembler MSP430CodeGen MSP430AsmParser MSP430Desc MSP430Info MipsDisassembler MipsCodeGen MipsAsmParser MipsDesc MipsInfo LanaiDisassembler LanaiCodeGen LanaiAsmParser LanaiDesc LanaiInfo HexagonDisassembler HexagonCodeGen HexagonAsmParser HexagonDesc HexagonInfo BPFDisassembler BPFCodeGen BPFAsmParser BPFDesc BPFInfo AVRDisassembler AVRCodeGen AVRAsmParser AVRDesc AVRInfo ARMDisassembler ARMCodeGen ARMAsmParser ARMDesc ARMUtils ARMInfo AMDGPUDisassembler AMDGPUCodeGen MIRParser ipo Instrumentation Vectorize Linker IRReader AsmParser FrontendOpenMP AMDGPUAsmParser AMDGPUDesc AMDGPUUtils AMDGPUInfo AArch64Disassembler MCDisassembler AArch64CodeGen CFGuard GlobalISel SelectionDAG AsmPrinter DebugInfoDWARF CodeGen Target ScalarOpts InstCombine AggressiveInstCombine TransformUtils BitWriter Analysis ProfileData Object TextAPI BitReader Core Remarks BitstreamReader AArch64AsmParser MCParser AArch64Desc MC DebugInfoCodeView DebugInfoMSF BinaryFormat AArch64Utils AArch64Info Support Demangle)
        #X86Utils)
# LLVM_DYLIB_COMPONENTS in case of dynamic lib
endif() 

# linenoise =================================================================== #
 FetchContent_Declare(
  linenoise
  GIT_REPOSITORY https://github.com/yhirose/cpp-linenoise.git
 #  GIT_TAG        1.2.1
  )

FetchContent_MakeAvailable(linenoise)

# fmt =================================================================== #
set (BUILD_SHARED_LIBS ON)
FetchContent_Declare(
  fmt
  GIT_REPOSITORY https://github.com/fmtlib/fmt.git
  GIT_TAG        10.2.1
)

FetchContent_MakeAvailable(fmt)

# pybind11 =================================================================== #
 FetchContent_Declare(
  pybind11
  GIT_REPOSITORY https://github.com/pybind/pybind11.git
#   GIT_TAG        2.6.1
  )
FetchContent_GetProperties(pybind11)

if(NOT pybind11_POPULATED)
  message(STATUS "Populating pybind11")
  FetchContent_Populate(pybind11)
  set(pybind11_INCLUDE_DIR ${pybind11_SOURCE_DIR}/include)
  add_library(pybind11 IMPORTED INTERFACE)
  set_property(TARGET pybind11 PROPERTY INTERFACE_INCLUDE_DIRECTORIES "${pybind11_INCLUDE_DIR}")
endif()

#---------------------------------------------------------------------------

if(NOT ${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pthread")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mbmi2")
  #this is for zminex, as it uses a special CPU instruction (bmi2) for fast member encoding (the morton / z-order curve)
  #without this zmindex does not compile 
  #CMAKE_CXX_FLAGS holds compiler flags
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -pthread")
  set(DYLIB_LIBRARY "-ldl")
else()
  set(DYLIB_LIBRARY "")
endif()

#---------------------------------------------------------------------------



######################
# Boost C++ library
######################
#
SET(BOOST_MIN_VERSION "1.70.0")

# get rid of an annoying warning
if(POLICY CMP0144)
   cmake_policy(SET CMP0144 NEW) # find_package() uses <PackageName>_ROOT variables
 endif()

find_package(Boost ${BOOST_MIN_VERSION} REQUIRED COMPONENTS
    program_options
    filesystem
    date_time
    thread)

if (NOT Boost_FOUND)
     message(FATAL_ERROR "Fatal error: Boost (version >= ${BOOST_MIN_VERSION}) required.\n"
 )
endif (NOT Boost_FOUND)

if (Boost_FOUND)
    include_directories(${Boost_INCLUDE_DIR})
    add_definitions( "-DHAS_BOOST" )
endif()

#-----------------------------------------------------------------------------------------

# include_directories("${pegtl_SOURCE_DIR}/include")
include_directories("${csv_SOURCE_DIR}")
include_directories("${spdlog_SOURCE_DIR}/include")
include_directories("${PROJECT_SOURCE_DIR}/src")
include_directories("${linenoise_SOURCE_DIR}")
include_directories("${fmt_SOURCE_DIR}/include")

###########################
# Python Interface
###########################
#
find_package(Python3 COMPONENTS Interpreter)

if (BUILD_PYTHON)
message(STATUS "PYTHON")
find_package (Python3 COMPONENTS Interpreter Development)
  if (Python3_Development_FOUND)
    message(STATUS "Python Version ${PYTHON_VERSION_MAJOR} found - building the Python API.")
    message("PythonInclude ${Python3_INCLUDE_DIRS}")
    message("PythonLibs ${Python3_LIBRARY_DIRS}")

    include_directories(${Python3_INCLUDE_DIRS})
    include_directories(${pybind11_INCLUDE_DIR})
    link_directories(${PYTHON_LIBDIR})

    set(PYTHON_SOURCES
      src/py/py_poseidon.cpp
    )
  else()
    set (PYTHON_SOURCES "")
  endif(Python3_Development_FOUND)
else ()
        message(STATUS "Don't build python interface")
        set (PYTHON_SOURCES "")
endif()

#-----------------------------------------------------------------------------------------

if(USE_GUNROCK MATCHES ON)
  set(gunrock_analysis 
    src/analytics/gunrock/gunrock_sssp.cpp
    src/analytics/gunrock/gunrock_bfs.cpp
    src/analytics/gunrock/gunrock_pr.cpp
  )
else()
  set(gunrock_analysis "")
endif()

if(USE_LLVM MATCHES ON)
  set(llvm_src 
  src/query/compile/compile_code_visitor.cpp
  src/query/compile/jit_compiler.cpp
  src/query/compile/jit_funcs.cpp
  src/query/compile/jit_engine.cpp
  src/query/compile/ir_generator.cpp
  src/query/compile/expr_codegen.cpp
  )
else()
  set(llvm_src "")
endif()

if(CSR_DELTA MATCHES ON)
  set(csr_delta_src src/storage/csr_delta.cpp)
else()
  set(csr_delta_src "")
endif()

set(src_files 
  src/dict/string_pool.cpp 
  src/dict/paged_string_pool.cpp 
  src/dict/code_table.cpp
  src/dict/dict.cpp
  src/storage/nodes.cpp
  src/storage/relationships.cpp
  src/storage/properties.cpp
  src/storage/graph_db.cpp
  src/storage/graph_db_import.cpp
  src/storage/graph_db_index.cpp
  src/storage/graph_db_learned_index.cpp
  src/storage/graph_db_gc.cpp
  src/storage/graph_db_recovery.cpp
  src/bp_file/paged_file.cpp
  src/bp_file/bufferpool.cpp
  src/bp_file/lru_list.cpp
  src/btree/index_map.cpp
  src/btree/dual_index.cpp
  src/query/plan_op/qop.cpp
  src/query/plan_op/qop_projection.cpp
  src/query/plan_op/qop_builtins.cpp
  src/query/plan_op/qop_scans.cpp
  src/query/plan_op/qop_relationships.cpp
  src/query/plan_op/qop_aggregates.cpp
  src/query/plan_op/qop_analytics.cpp
  src/query/plan_op/qop_algorithm.cpp
  src/query/plan_op/qop_updates.cpp
  src/query/plan_op/qop_joins.cpp
  src/query/plan_op/qop_set.cpp
  src/query/plan_op/query_batch.cpp
  src/query/plan_op/query_pipeline.cpp
  src/query/plan_op/query_builder.cpp
  src/query/expr/binary_expression.cpp
  src/query/expr/expression.cpp
  src/query/interp/expr_interpreter.cpp
  src/query/interp/qinterp.cpp
  src/query/interp/query_ctx.cpp
  src/query/utils/query_printer.cpp
  src/query/utils/thread_pool.cpp
  src/query/utils/profiling.cpp
  src/query/utils/qresult.cpp
  src/query/utils/qresult_iterator.cpp
  src/query/plan_visitors/prepare_expr_visitor.cpp
  src/query/plan_visitors/prepare_plan_visitor.cpp
  src/tx/transaction.cpp
  src/pool/graph_pool.cpp
  src/log/pm_ulog.cpp
  src/log/walog.cpp
  ${llvm_src}
  src/query/parser/query_planner.cpp
  src/query/parser/query_proc.cpp
  ${csr_delta_src}
  ${gunrock_analysis}
  src/analytics/bfs.cpp
  src/analytics/shortest_path.cpp
  src/analytics/graph_db_analytics.cpp
  ${ANTLR_grammar_CXX_OUTPUTS}
)

if(USE_GUNROCK MATCHES ON)
  set(cu_files
    src/analytics/graph_db_analytics.cu
  )
else()
  set(cu_files "")
endif()

# Using OBJECT to avoid to build object files multiple times due to inclusion
# in different libraries.
# macOS doesn't like SHARED, Linux needs it
if(${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
add_library(poseidon_obj OBJECT ${src_files} ${cu_files})
else()
add_library(poseidon_obj OBJECT SHARED ${src_files} ${cu_files})
endif()

# Poseidon core library for building C++ applications
add_library(poseidon_core SHARED
  $<TARGET_OBJECTS:poseidon_obj>
)

if(USE_GUNROCK MATCHES ON)
  set(CUDA_LIB ${CUDA_LIBRARIES})
  set(gunrock_linkLib gunrock)
else()
  set(CUDA_LIB "")
  set(gunrock_linkLib "")
endif()

if(USE_LLVM MATCHES ON)
  set(LLVM_LIB LLVM)
else()
  set(LLVM_LIB "")
endif()

target_link_libraries(poseidon_core
  ${LLVM_LIB}
  antlr4_cpp_runtime
#  antlr4_shared
  Boost::filesystem
  Boost::date_time
  Boost::thread
  spdlog::spdlog_header_only
  fmt::fmt
  ${gunrock_linkLib}
  ${CUDA_LIB}
)


#add_custom_command(TARGET poseidon_core
#                   POST_BUILD
#                   COMMAND ${CMAKE_COMMAND}
#                           -E copy ${ANTLR4_RUNTIME_LIBRARIES} .
#                   WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})


target_include_directories( ${PROJECT_NAME}
    PUBLIC ${PROJECT_SOURCE_DIR}/include
)

if (BUILD_PYTHON)
message("Build python ${PYTHON_SOURCES}")
# Python Poseidon library for loading a module into the python3 interpreter
add_library(poseidon SHARED
  $<TARGET_OBJECTS:poseidon_obj>
  ${PYTHON_SOURCES}
)

target_link_libraries(poseidon
  ${LLVM_LIB}
  antlr4_cpp_runtime
  # antlr4_shared
  Boost::filesystem
  Boost::date_time
  Boost::thread
  ${Python3_LIBRARIES}
  ${PMDK_LIBRARIES}
  spdlog::spdlog_header_only
  fmt::fmt
)

set_target_properties(poseidon PROPERTIES PREFIX "" SUFFIX ".so")
endif()

#-----------------------------------------------------------------------------------------

add_subdirectory(src/antlr4_cpp_runtime)

#-----------------------------------------------------------------------------------------

add_executable(pcli
  src/cmds/pcli.cpp
 )

 target_link_libraries(pcli
  poseidon_core
  ${PMDK_LIBRARIES}
  Boost::program_options
  ${CMAKE_DL_LIBS}
)

add_executable(gtpc_driver
  src/cmds/gtpc_driver.cpp
)

target_link_libraries(gtpc_driver
  poseidon_core
  ${PMDK_LIBRARIES}
  Boost::program_options
)

add_executable(wal_dump
  src/cmds/wal_dump.cpp
)

target_link_libraries(wal_dump
  poseidon_core
  ${PMDK_LIBRARIES}
  Boost::program_options
 )

if(USE_UPMEM MATCHES ON)
add_executable(dpu_host
  src/cmds/dpu.cpp
  # src/upmem/scan/host/dpu_scan.cpp
  src/upmem/aggregation/host/utils.cpp
  # src/upmem/aggregation/host/hash_aggr_hi_card_v1.cpp
  # src/upmem/aggregation/host/hash_aggr_hi_card_v2.cpp
  # src/upmem/aggregation/host/hash_aggr_hi_card_v3.cpp
  # src/upmem/aggregation/host/hash_aggr_hi_card_v4.cpp
  src/upmem/aggregation/host/hash_aggr_hi_card_v5.cpp
  # src/upmem/aggregation/host/hash_aggr_low_card.cpp
  # src/upmem/aggregation/host/sort_aggr_hi_card.cpp
 )

 target_link_libraries(dpu_host
 poseidon_core
 ${PMDK_LIBRARIES}
 Boost::program_options
 ${DPU_LIB}
)
endif()

#-----------------------------------------------------------------------------------------
#
# Unit tests using Catch
#
enable_testing()
add_subdirectory(test)

#-----------------------------------------------------------------------------------------

# Create logs directory
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/logs")


add_executable(test_dualindex test/dual_index_test.cpp)
target_link_libraries(test_dualindex poseidon_core)
target_compile_options(test_dualindex PRIVATE -mbmi2)

add_executable(bench_dualindex test/bench_dualindex_wiki.cpp)
target_link_libraries(bench_dualindex poseidon_core)
target_compile_options(bench_dualindex PRIVATE -mbmi2 -O3 -DNDEBUG)
# Note: test/router_e2e_test.cpp is auto-registered by test/CMakeLists.txt's
# *_test.cpp glob (target: router_e2e_test); -mbmi2 is applied globally (line 295).