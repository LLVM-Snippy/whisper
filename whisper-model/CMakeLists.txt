cmake_minimum_required(VERSION 3.20)

project(SnippyWhisperModel LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT DEFINED WHISPER_ROOT)
  message(FATAL_ERROR "WHISPER_ROOT must be provided by the parent CMakeLists.txt")
endif()

if(NOT WHISPER_COMPLETE_LIBRARY)
  message(FATAL_ERROR "WHISPER_COMPLETE_LIBRARY must be provided by the parent CMakeLists.txt")
endif()

add_library(SnippyWhisperModel SHARED
  ${CMAKE_CURRENT_SOURCE_DIR}/src/RVMExports.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/SnippyWhisperModel.cpp

  # Keep these explicit until Whisper provides a self-contained CMake/static library target.
  ${WHISPER_ROOT}/virtual_memory/Tlb.cpp
  ${WHISPER_ROOT}/virtual_memory/VirtMem.cpp
  ${WHISPER_ROOT}/pci/Pci.cpp
  ${WHISPER_ROOT}/pci/virtio/Blk.cpp
  ${WHISPER_ROOT}/pci/virtio/Virtio.cpp
)

target_include_directories(SnippyWhisperModel
  PUBLIC
    ${CMAKE_INCLUDE_PATH}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${WHISPER_ROOT}
    ${WHISPER_ROOT}/virtual_memory
    ${WHISPER_ROOT}/pci
    ${WHISPER_ROOT}/pci/virtio
)

find_package(Threads REQUIRED)
find_library(UTIL_LIBRARY util)

target_link_libraries(SnippyWhisperModel
  PRIVATE
    ${WHISPER_COMPLETE_LIBRARY}
    Threads::Threads
    ${UTIL_LIBRARY}
)

if(WHISPER_SOFTFLOAT_LIBRARY)
  target_link_libraries(SnippyWhisperModel PRIVATE ${WHISPER_SOFTFLOAT_LIBRARY})
endif()

find_package(ZLIB REQUIRED)
target_link_libraries(SnippyWhisperModel PRIVATE Boost::boost ZLIB::ZLIB)

find_library(LZ4_LIBRARY lz4)
if(LZ4_LIBRARY)
  target_link_libraries(SnippyWhisperModel PRIVATE ${LZ4_LIBRARY})
endif()

# Catch unresolved dependencies at build time rather than at llvm-snippy dlopen time.
target_link_options(SnippyWhisperModel PRIVATE "-Wl,--no-undefined")

set_target_properties(SnippyWhisperModel PROPERTIES
  OUTPUT_NAME "RISCVModel"
)
