cmake_minimum_required(VERSION 3.20)

project(WhisperWithSnippyModel LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# This CMakeLists.txt is expected to live in the root of the Whisper checkout.
#
# Layout:
#   whisper/
#     CMakeLists.txt
#     GNUmakefile
#     whisper-model/
#       CMakeLists.txt
#       include/
#       src/

set(WHISPER_ROOT "${CMAKE_CURRENT_LIST_DIR}")
set(WHISPER_BUILD_DIR "${WHISPER_ROOT}/build-Linux")

set(WHISPER_RVCORE_LIBRARY
    "${WHISPER_BUILD_DIR}/librvcore.a")

set(WHISPER_COMPLETE_LIBRARY
    "${WHISPER_BUILD_DIR}/libwhisper_complete.a")

set(WHISPER_SOFTFLOAT_LIBRARY
    "${WHISPER_ROOT}/third_party/softfloat/build/RISCV-GCC/softfloat.a")

message(STATUS "WHISPER_ROOT=${WHISPER_ROOT}")
message(STATUS "WHISPER_BUILD_DIR=${WHISPER_BUILD_DIR}")
message(STATUS "WHISPER_RVCORE_LIBRARY=${WHISPER_RVCORE_LIBRARY}")
message(STATUS "WHISPER_COMPLETE_LIBRARY=${WHISPER_COMPLETE_LIBRARY}")
message(STATUS "WHISPER_SOFTFLOAT_LIBRARY=${WHISPER_SOFTFLOAT_LIBRARY}")

if(NOT EXISTS "${WHISPER_ROOT}/GNUmakefile")
  message(FATAL_ERROR
          "Whisper GNUmakefile was not found at ${WHISPER_ROOT}/GNUmakefile. "
          "Put this CMakeLists.txt into the Whisper checkout root.")
endif()

find_package(Boost REQUIRED)

# Whisper's GNUmakefile wants BOOST_DIR, not Boost_INCLUDE_DIRS.
#
# Convert common include paths:
#   /usr/include                 -> /usr
#   /opt/boost/include           -> /opt/boost
#   /opt/boost/include/boost     -> /opt/boost
set(WHISPER_BOOST_DIR "${Boost_INCLUDE_DIRS}")

if(WHISPER_BOOST_DIR MATCHES "/include/boost/?$")
  get_filename_component(WHISPER_BOOST_DIR "${WHISPER_BOOST_DIR}" DIRECTORY)
endif()

if(WHISPER_BOOST_DIR MATCHES "/include/?$")
  get_filename_component(WHISPER_BOOST_DIR "${WHISPER_BOOST_DIR}" DIRECTORY)
endif()

message(STATUS "WHISPER_BOOST_DIR=${WHISPER_BOOST_DIR}")

include(CheckCompilerFlag)

# Check for C++20 flag support across different compilers
check_compiler_flag(CXX "-std=c++20" COMPILER_SUPPORTS_CXX20)
if (NOT COMPILER_SUPPORTS_CXX20)
find_program(SNIPPY_WHISPER_CXX NAMES g++-13 REQUIRED)
find_program(SNIPPY_WHISPER_CC  NAMES gcc-13 REQUIRED)
set(CMAKE_CXX_COMPILER "${SNIPPY_WHISPER_CXX}"
    CACHE FILEPATH "C++ compiler" FORCE)

set(CMAKE_C_COMPILER "${SNIPPY_WHISPER_CC}"
    CACHE FILEPATH "C compiler" FORCE)
endif()

set(WHISPER_CXXFLAGS "-std=c++20 -fPIC")
set(WHISPER_CFLAGS "-fPIC")

message(STATUS "CMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}")
message(STATUS "CMAKE_C_COMPILER=${CMAKE_C_COMPILER}")

# Build SoftFloat through Whisper's native GNUmakefile.
#
# This must be a real custom command with OUTPUT. Do not list softfloat.a only
# as a DEPENDS of a custom target: fresh CI builds will not know how to create it.
add_custom_command(
  OUTPUT "${WHISPER_SOFTFLOAT_LIBRARY}"
  COMMAND "${CMAKE_COMMAND}" -E echo
          "Building Whisper SoftFloat: ${WHISPER_SOFTFLOAT_LIBRARY}"
  COMMAND "${CMAKE_COMMAND}" -E env
          "CC=${CMAKE_C_COMPILER}"
          "CXX=${CMAKE_CXX_COMPILER}"
          "CFLAGS=${WHISPER_CFLAGS}"
          "CXXFLAGS=${WHISPER_CXXFLAGS}"
          "AR=${CMAKE_AR}"
          "BOOST_DIR=${WHISPER_BOOST_DIR}"
          "BOOST_ROOT=${WHISPER_BOOST_DIR}"
          "${CMAKE_MAKE_PROGRAM}"
          "${WHISPER_SOFTFLOAT_LIBRARY}"
          "CC=${CMAKE_C_COMPILER}"
          "CXX=${CMAKE_CXX_COMPILER}"
          "AR=${CMAKE_AR}"
          "BOOST_DIR=${WHISPER_BOOST_DIR}"
          "BOOST_ROOT=${WHISPER_BOOST_DIR}"
  WORKING_DIRECTORY "${WHISPER_ROOT}"
  DEPENDS "${WHISPER_ROOT}/GNUmakefile"
  VERBATIM
)

add_custom_target(whisper_softfloat
  DEPENDS "${WHISPER_SOFTFLOAT_LIBRARY}"
)

# Build Whisper core with its native GNUmakefile, but do not build the CLI executable.
#
# Building 'all' may try to link the whisper binary and fail on Args/Session/HartConfig,
# while the snippy model only needs the core object files/archive.
add_custom_command(
  OUTPUT "${WHISPER_COMPLETE_LIBRARY}"
  COMMAND "${CMAKE_COMMAND}" -E echo
          "Building Whisper core library: ${WHISPER_RVCORE_LIBRARY}"
  COMMAND "${CMAKE_COMMAND}" -E env
          "CC=${CMAKE_C_COMPILER}"
          "CXX=${CMAKE_CXX_COMPILER}"
          "CFLAGS=${WHISPER_CFLAGS}"
          "CXXFLAGS=${WHISPER_CXXFLAGS}"
          "AR=${CMAKE_AR}"
          "BOOST_DIR=${WHISPER_BOOST_DIR}"
          "BOOST_ROOT=${WHISPER_BOOST_DIR}"
          "${CMAKE_MAKE_PROGRAM}"
          "build-Linux/librvcore.a"
          "VERBOSE=1"
          "-j12"

  COMMAND "${CMAKE_COMMAND}" -E echo
          "Assembling complete archive: ${WHISPER_COMPLETE_LIBRARY}"

  COMMAND /bin/sh -c
          "find '${WHISPER_BUILD_DIR}' -name '*.o' \
             ! -name 'main.o' \
             ! -name 'main.cpp.o' \
             ! -name 'whisper.o' \
             ! -name 'whisper.cpp.o' \
             ! -name 'Driver.o' \
             ! -name 'driver.o' \
             ! -name '*Test*.o' \
             ! -name '*test*.o' \
             > '${CMAKE_BINARY_DIR}/whisper_objects.txt'"

  COMMAND /bin/sh -c
          "test -s '${CMAKE_BINARY_DIR}/whisper_objects.txt' || \
           { echo 'No Whisper object files found under ${WHISPER_BUILD_DIR}' >&2; exit 1; }"

  COMMAND /bin/sh -c
          "${CMAKE_AR} rcs '${WHISPER_COMPLETE_LIBRARY}' @'${CMAKE_BINARY_DIR}/whisper_objects.txt'"

  COMMAND "${CMAKE_RANLIB}" "${WHISPER_COMPLETE_LIBRARY}"

  WORKING_DIRECTORY "${WHISPER_ROOT}"
  DEPENDS
    "${WHISPER_ROOT}/GNUmakefile"
    whisper_softfloat
  VERBATIM
)

add_custom_target(whisper_complete_archive ALL
  DEPENDS "${WHISPER_COMPLETE_LIBRARY}"
)

# Export paths to the model subdirectory.
set(WHISPER_ROOT "${WHISPER_ROOT}"
    CACHE INTERNAL "Whisper source root")

set(WHISPER_COMPLETE_LIBRARY "${WHISPER_COMPLETE_LIBRARY}"
    CACHE INTERNAL "Complete Whisper static archive")

set(WHISPER_SOFTFLOAT_LIBRARY "${WHISPER_SOFTFLOAT_LIBRARY}"
    CACHE INTERNAL "Whisper SoftFloat archive")

if(NOT EXISTS "${WHISPER_ROOT}/whisper-model/CMakeLists.txt")
  message(FATAL_ERROR
          "Expected ${WHISPER_ROOT}/whisper-model/CMakeLists.txt")
endif()

add_subdirectory(whisper-model "${CMAKE_BINARY_DIR}/whisper-model-build")

add_dependencies(SnippyWhisperModel
  whisper_complete_archive
  whisper_softfloat
)
