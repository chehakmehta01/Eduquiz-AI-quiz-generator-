cmake_minimum_required(VERSION 3.16)
project(EduPulseAI CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------- 1. FLTK : the library that draws windows/buttons/tabs ----------
include(FetchContent)
set(FLTK_BUILD_TEST      OFF CACHE BOOL "" FORCE)
set(FLTK_BUILD_EXAMPLES  OFF CACHE BOOL "" FORCE)
set(FLTK_BUILD_FLUID     OFF CACHE BOOL "" FORCE)
set(FLTK_BUILD_FLTK_OPTIONS OFF CACHE BOOL "" FORCE)
set(FLTK_BUILD_GL        OFF CACHE BOOL "" FORCE)
set(FLTK_BUILD_FORMS     OFF CACHE BOOL "" FORCE)
set(FLTK_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  fltk
  GIT_REPOSITORY https://github.com/fltk/fltk.git
  GIT_TAG        branch-1.4
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(fltk)

# ---------- 2. Two single-header helper libraries (downloaded once) ----------
set(DEPS_DIR ${CMAKE_BINARY_DIR}/deps)
if(NOT EXISTS ${DEPS_DIR}/httplib.h)
  message(STATUS "Downloading httplib.h (talks to Ollama over HTTP)...")
  file(DOWNLOAD https://raw.githubusercontent.com/yhirose/cpp-httplib/v0.18.0/httplib.h
       ${DEPS_DIR}/httplib.h TLS_VERIFY ON)
endif()
if(NOT EXISTS ${DEPS_DIR}/json.hpp)
  message(STATUS "Downloading json.hpp (reads the AI's JSON answer)...")
  file(DOWNLOAD https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp
       ${DEPS_DIR}/json.hpp TLS_VERIFY ON)
endif()

# ---------- 3. Our program ----------
if(WIN32)
  add_executable(edupulse WIN32 main.cpp)   # WIN32 = no black console window
else()
  add_executable(edupulse main.cpp)
endif()

target_include_directories(edupulse PRIVATE ${DEPS_DIR})
target_compile_definitions(edupulse PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

find_package(Threads REQUIRED)
target_link_libraries(edupulse PRIVATE fltk Threads::Threads)
if(WIN32)
  target_link_libraries(edupulse PRIVATE ws2_32)
endif()
