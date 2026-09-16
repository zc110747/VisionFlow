cmake_minimum_required(VERSION 3.16)
project(camera-agent CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(CAMERA_AGENT_BUILD_TESTS "Build the test suite" ON)
set(CAMERA_AGENT_BACKEND "auto" CACHE STRING "Pipeline backend: gstreamer|sim|auto")

add_compile_definitions(CAMERA_AGENT_VERSION="0.1.0")

# ---- spdlog (mature logging library, fetched from GitHub) -------------------
include(FetchContent)
FetchContent_Declare(spdlog
  GIT_REPOSITORY https://github.com/gabime/spdlog.git
  GIT_TAG v1.14.1
  GIT_SHALLOW ON)
FetchContent_MakeAvailable(spdlog)

# ---- GStreamer detection (only needed for the real backend) -----------------
# Portable by design: NO machine-specific path is committed to this file.
#
# The install root is resolved in this order (first hit wins):
#   1. -DGSTREAMER_ROOT=<path>               explicit, per machine, never committed
#   2. $ENV{GSTREAMER_ROOT}                  recommended per-machine variable
#   3. $ENV{GSTREAMER_1_0_ROOT_MSVC_X86_64__Plus}
#      $ENV{GSTREAMER_1_0_ROOT_MSVC_X86_64}  variables the official installer sets
#      $ENV{GSTREAMER_1_0_ROOT_X86_64}
#   4. gst-launch-1.0 / gst-inspect-1.0 / pkg-config found on PATH -> root = <bin>/..
#   5. a few generic default prefixes
#
# Both layouts are accepted and auto-detected:
#     <root>/include/gstreamer-1.0/gst/gst.h          (flat)
#     <root>/1.0/msvc_x86_64/include/gstreamer-1.0/   (versioned)
# A candidate is accepted only if the headers really exist there, so a wrong
# path is rejected instead of silently producing confusing compile errors.
set(HAVE_GSTREAMER OFF)
set(GSTREAMER_ROOT "" CACHE PATH
    "GStreamer 1.0 install root (the directory holding bin/, lib/ and include/)")

# Validate a candidate root; return the usable root, or "" when unusable.
function(_gst_resolve_root _in _out)
  set(_r "")
  if(_in)
    string(REPLACE "\\" "/" _p "${_in}")
    string(REGEX REPLACE "/+$" "" _p "${_p}")
    foreach(_c "${_p}" "${_p}/1.0/msvc_x86_64" "${_p}/1.0/mingw_x86_64" "${_p}/1.0/x86_64")
      if(EXISTS "${_c}/include/gstreamer-1.0/gst/gst.h")
        set(_r "${_c}")
        break()
      endif()
    endforeach()
  endif()
  set(${_out} "${_r}" PARENT_SCOPE)
endfunction()

set(_gst_root "")
set(_gst_origin "")

# 1) explicit cache variable
if(GSTREAMER_ROOT)
  _gst_resolve_root("${GSTREAMER_ROOT}" _gst_root)
  if(_gst_root)
    set(_gst_origin "-DGSTREAMER_ROOT")
  endif()
endif()

# 2) + 3) environment variables
if(NOT _gst_root)
  foreach(_v IN ITEMS GSTREAMER_ROOT
                      GSTREAMER_1_0_ROOT_MSVC_X86_64__Plus
                      GSTREAMER_1_0_ROOT_MSVC_X86_64
                      GSTREAMER_1_0_ROOT_X86_64
                      GSTREAMER_ROOT_X86_64)
    if(NOT "$ENV{${_v}}" STREQUAL "")
      _gst_resolve_root("$ENV{${_v}}" _gst_root)
      if(_gst_root)
        set(_gst_origin "environment variable ${_v}")
        break()
      endif()
    endif()
  endforeach()
endif()

# 4) derive from a GStreamer tool that is already on PATH
if(NOT _gst_root)
  foreach(_tool gst-launch-1.0 gst-inspect-1.0 pkg-config)
    unset(_gst_tool CACHE)
    find_program(_gst_tool NAMES ${_tool})
    if(_gst_tool)
      get_filename_component(_gst_bindir "${_gst_tool}" DIRECTORY)
      get_filename_component(_gst_parent "${_gst_bindir}" DIRECTORY)
      _gst_resolve_root("${_gst_parent}" _gst_root)
      if(_gst_root)
        set(_gst_origin "${_tool} on PATH")
        break()
      endif()
    endif()
  endforeach()
endif()

# 5) generic defaults (deliberately free of user/machine specific directories)
if(NOT _gst_root)
  set(_gst_prefixes
      "$ENV{ProgramFiles}/gstreamer/1.0/msvc_x86_64"
      "$ENV{ProgramFiles}/gstreamer"
      "$ENV{LOCALAPPDATA}/Programs/gstreamer/1.0/msvc_x86_64"
      "C:/gstreamer/1.0/msvc_x86_64"
      "C:/gstreamer"
      "/usr/local"
      "/usr")
  foreach(_p ${_gst_prefixes})
    _gst_resolve_root("${_p}" _gst_root)
    if(_gst_root)
      set(_gst_origin "default prefix")
      break()
    endif()
  endforeach()
endif()

if(_gst_root)
  set(GSTREAMER_ROOT "${_gst_root}" CACHE PATH
      "GStreamer 1.0 install root (the directory holding bin/, lib/ and include/)" FORCE)
  message(STATUS "GStreamer root: ${GSTREAMER_ROOT}  (located via ${_gst_origin})")
else()
  message(STATUS "GStreamer root: <not found>")
endif()

if(CAMERA_AGENT_BACKEND STREQUAL "gstreamer" OR CAMERA_AGENT_BACKEND STREQUAL "auto")
  if(GSTREAMER_ROOT)
    set(GST_INCLUDE_DIRS "")
    set(GST_LIBRARY_DIRS "")
    set(GST_LIBRARIES "")

    # -- include dirs ------------------------------------------------------
    # pkg-config is authoritative: it knows about glib-2.0, the architecture
    # specific lib/glib-2.0/include directory, gio, etc. GStreamer ships its
    # own pkg-config, and its .pc files are relocatable (${pcfiledir}/../..),
    # so this works no matter where the SDK was installed.
    unset(PKG_CONFIG_EXECUTABLE CACHE)
    find_program(PKG_CONFIG_EXECUTABLE NAMES pkg-config.exe pkg-config
                 HINTS "${GSTREAMER_ROOT}/bin" NO_DEFAULT_PATH)
    if(NOT PKG_CONFIG_EXECUTABLE)
      unset(PKG_CONFIG_EXECUTABLE CACHE)
      find_program(PKG_CONFIG_EXECUTABLE NAMES pkg-config.exe pkg-config)
    endif()

    set(_gst_pc_used OFF)
    if(PKG_CONFIG_EXECUTABLE)
      # Make the .pc files discoverable regardless of the caller's environment.
      if(NOT "$ENV{PKG_CONFIG_PATH}" STREQUAL "")
        set(ENV{PKG_CONFIG_PATH}
            "${GSTREAMER_ROOT}/lib/pkgconfig;$ENV{PKG_CONFIG_PATH}")
      else()
        set(ENV{PKG_CONFIG_PATH} "${GSTREAMER_ROOT}/lib/pkgconfig")
      endif()
      find_package(PkgConfig QUIET)
      if(PkgConfig_FOUND)
        pkg_check_modules(PC_GST QUIET
          gstreamer-1.0>=1.14 gstreamer-app-1.0 gstreamer-rtsp-1.0
          gstreamer-video-1.0 gstreamer-base-1.0 gobject-2.0 glib-2.0 gio-2.0)
        if(PC_GST_FOUND)
          set(_gst_pc_used ON)
          set(GST_INCLUDE_DIRS ${PC_GST_INCLUDE_DIRS})
          set(GST_LIBRARY_DIRS ${PC_GST_LIBRARY_DIRS})
        endif()
      endif()
    endif()

    if(NOT _gst_pc_used)
      # Fallback: the standard GStreamer/GLib layout, verified directory by
      # directory. gio/gio.h and glibconfig.h both need their own entries.
      foreach(_d "${GSTREAMER_ROOT}/include/gstreamer-1.0"
                 "${GSTREAMER_ROOT}/include/glib-2.0"
                 "${GSTREAMER_ROOT}/lib/glib-2.0/include")
        if(IS_DIRECTORY "${_d}")
          list(APPEND GST_INCLUDE_DIRS "${_d}")
        endif()
      endforeach()
      # glibconfig.h is architecture dependent; be sure we picked its directory.
      find_path(_gst_glibcfg glibconfig.h
                HINTS "${GSTREAMER_ROOT}/lib/glib-2.0/include"
                      "${GSTREAMER_ROOT}/lib/glib-2.0" NO_DEFAULT_PATH)
      if(_gst_glibcfg)
        list(APPEND GST_INCLUDE_DIRS "${_gst_glibcfg}")
      endif()
      if(GST_INCLUDE_DIRS)
        list(REMOVE_DUPLICATES GST_INCLUDE_DIRS)
      endif()
      set(GST_LIBRARY_DIRS "${GSTREAMER_ROOT}/lib")
    endif()

    # -- import libraries: resolve every name to an absolute file ------------
    set(_gst_missing "")
    foreach(_l gstreamer-1.0 gstapp-1.0 gstrtspserver-1.0 gstvideo-1.0
                gstbase-1.0 gobject-2.0 glib-2.0 gio-2.0)
      unset(_gst_lib_${_l} CACHE)
      find_library(_gst_lib_${_l} NAMES ${_l}
                   HINTS ${GST_LIBRARY_DIRS} "${GSTREAMER_ROOT}/lib" NO_DEFAULT_PATH)
      if(NOT _gst_lib_${_l})
        find_library(_gst_lib_${_l} NAMES ${_l})
      endif()
      if(_gst_lib_${_l})
        list(APPEND GST_LIBRARIES "${_gst_lib_${_l}}")
      else()
        string(APPEND _gst_missing " ${_l}")
      endif()
    endforeach()

    # -- sanity check: gio/gio.h must be reachable ---------------------------
    set(_gst_gio_ok OFF)
    foreach(_d ${GST_INCLUDE_DIRS})
      if(EXISTS "${_d}/gio/gio.h")
        set(_gst_gio_ok ON)
      endif()
    endforeach()

    if(GST_INCLUDE_DIRS AND NOT _gst_missing AND _gst_gio_ok)
      set(HAVE_GSTREAMER ON)
      set(GSTREAMER_RUNTIME_DIR "${GSTREAMER_ROOT}/bin")
      message(STATUS "GStreamer: include dirs = ${GST_INCLUDE_DIRS}")
      if(_gst_pc_used)
        message(STATUS "GStreamer: flags resolved with pkg-config")
      endif()
    else()
      if(_gst_missing)
        message(STATUS "GStreamer: missing import libraries:${_gst_missing}")
      endif()
      if(NOT _gst_gio_ok)
        message(STATUS "GStreamer: gio/gio.h is not reachable from ${GST_INCLUDE_DIRS}")
      endif()
    endif()
  endif()
endif()

# ---- ONNX Runtime detection (optional; powers the AI branch) ----------------
# The AI branch is fully optional: when ONNX Runtime is not found the project
# still builds and runs, the only difference is that the detector cannot load a
# model (video streaming is unaffected by design - spec 19).
#
# Root resolution order (first hit wins):
#   1. -DONNXRUNTIME_ROOT=<path>   explicit, per machine, never committed
#   2. $ENV{ONNXRUNTIME_ROOT}      recommended per-machine variable
#   3. a few generic default prefixes
# A candidate is accepted only if include/onnxruntime_cxx_api.h really exists,
# so a wrong path is rejected instead of producing confusing compile errors.
option(CAMERA_AGENT_ENABLE_AI "Enable the AI branch (person detection + tracking)" ON)
set(ONNXRUNTIME_ROOT "" CACHE PATH
    "ONNX Runtime install root (the directory holding include/ and lib/)")

set(HAVE_ORT OFF)

if(CAMERA_AGENT_ENABLE_AI)
  set(_ort_root "")
  foreach(_c "${ONNXRUNTIME_ROOT}" "$ENV{ONNXRUNTIME_ROOT}"
             "D:/Software/onnxruntime" "C:/onnxruntime"
             "$ENV{ProgramFiles}/onnxruntime" "/usr/local" "/opt/onnxruntime")
    if(NOT _c STREQUAL "")
      string(REPLACE "\\" "/" _p "${_c}")
      string(REGEX REPLACE "/+$" "" _p "${_p}")
      if(EXISTS "${_p}/include/onnxruntime_cxx_api.h")
        set(_ort_root "${_p}")
        break()
      endif()
    endif()
  endforeach()

  if(_ort_root)
    unset(_ort_lib CACHE)
    find_library(_ort_lib NAMES onnxruntime
                 HINTS "${_ort_root}/lib" "${_ort_root}" NO_DEFAULT_PATH)
    if(NOT _ort_lib)
      unset(_ort_lib CACHE)
      find_library(_ort_lib NAMES onnxruntime)
    endif()
    if(_ort_lib)
      set(HAVE_ORT ON)
      set(ORT_INCLUDE_DIRS "${_ort_root}/include")
      set(ORT_LIBRARIES "${_ort_lib}")
      set(ORT_RUNTIME_DIR "${_ort_root}/lib")
      message(STATUS "ONNX Runtime: ${_ort_root}")
    else()
      message(STATUS "ONNX Runtime: headers found at ${_ort_root}, "
                     "but no import library (AI will not load a model)")
    endif()
  else()
    message(STATUS "ONNX Runtime: <not found> "
                   "(set -DONNXRUNTIME_ROOT=<path> to enable the AI branch)")
  endif()
endif()

# ---- Resolve backend --------------------------------------------------------
if(CAMERA_AGENT_BACKEND STREQUAL "auto")
  if(HAVE_GSTREAMER)
    set(CAMERA_AGENT_BACKEND "gstreamer")
  else()
    set(CAMERA_AGENT_BACKEND "sim")
  endif()
endif()

if(CAMERA_AGENT_BACKEND STREQUAL "gstreamer" AND NOT HAVE_GSTREAMER)
  message(FATAL_ERROR
    "The GStreamer backend was requested, but no usable GStreamer 1.0 "
    "installation was found.\n"
    "Fix it in one of these ways (nothing is hardcoded in this project):\n"
    "  cmake -S . -B build -DCAMERA_AGENT_BACKEND=gstreamer "
    "-DGSTREAMER_ROOT=\"D:/software/msvc_x86_64/gstreamer\"\n"
    "  # or, once per machine, set the environment variable\n"
    "  #   GSTREAMER_ROOT = D:\\software\\msvc_x86_64\\gstreamer\n"
    "  # and add %GSTREAMER_ROOT%\\bin to PATH so the DLLs load at runtime\n"
    "  # or build the dependency-free simulation backend instead:\n"
    "  cmake -S . -B build -DCAMERA_AGENT_BACKEND=sim")
endif()

message(STATUS "Camera-Agent backend selected: ${CAMERA_AGENT_BACKEND}")

add_subdirectory(src)

if(CAMERA_AGENT_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
