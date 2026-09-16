set(SOURCES
  main.cpp
  config/config.cpp
  common/stream_controller.cpp
  common/http_client_winhttp.cpp
  # AI branch (backend independent: the detector picks its implementation at
  # compile time from CAMERA_AGENT_HAVE_ORT).
  ai/ai_pipeline.cpp
  ai/byte_tracker.cpp
  ai/yolo_detector.cpp
  # Metadata branch (Phase 2): encode -> bounded queue -> async sender thread.
  metadata/metadata_encoder.cpp
  metadata/metadata_manager.cpp
  metadata/http_transport_winhttp.cpp
)

if(CAMERA_AGENT_BACKEND STREQUAL "gstreamer")
  list(APPEND SOURCES
    camera/camera_manager_gst.cpp
    pipeline/video_pipeline_gst.cpp
    rtsp/rtsp_publisher_gst.cpp
  )
else()
  list(APPEND SOURCES
    camera/camera_manager_sim.cpp
    pipeline/video_pipeline_sim.cpp
    rtsp/rtsp_publisher_sim.cpp
  )
endif()

add_executable(camera-agent ${SOURCES})

target_include_directories(camera-agent PRIVATE
  ${PROJECT_SOURCE_DIR}/include)

if(MSVC)
  # /utf-8: sources are UTF-8; without it MSVC reads them as GBK and emits C4819.
  target_compile_options(camera-agent PRIVATE /W4 /EHsc /utf-8)
else()
  target_compile_options(camera-agent PRIVATE -Wall -Wextra)
endif()

target_link_libraries(camera-agent PRIVATE spdlog::spdlog)

# Metadata transport: WinHTTP is an OS component, so there is nothing to install.
if(WIN32)
  target_link_libraries(camera-agent PRIVATE winhttp)
endif()

if(HAVE_ORT)
  target_include_directories(camera-agent PRIVATE ${ORT_INCLUDE_DIRS})
  target_link_libraries(camera-agent PRIVATE ${ORT_LIBRARIES})
  target_compile_definitions(camera-agent PRIVATE CAMERA_AGENT_HAVE_ORT)
  # The prebuilt Windows package ships the import library next to the DLLs, so
  # copy them beside the executable: no machine-specific PATH edit needed.
  set(_ort_dlls "")
  foreach(_d onnxruntime.dll onnxruntime_providers_shared.dll)
    if(EXISTS "${ORT_RUNTIME_DIR}/${_d}")
      list(APPEND _ort_dlls "${ORT_RUNTIME_DIR}/${_d}")
    endif()
  endforeach()
  if(_ort_dlls)
    add_custom_command(TARGET camera-agent POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_ort_dlls}
              $<TARGET_FILE_DIR:camera-agent>
      COMMENT "Copying ONNX Runtime DLLs next to camera-agent")
  endif()
else()
  message(STATUS "AI branch: compiled WITHOUT a model backend "
                 "(only the pipeline/tracker are active)")
endif()

if(CAMERA_AGENT_BACKEND STREQUAL "gstreamer")
  target_include_directories(camera-agent PRIVATE ${GST_INCLUDE_DIRS})
  target_link_libraries(camera-agent PRIVATE ${GST_LIBRARIES})
  target_compile_definitions(camera-agent PRIVATE
    CAMERA_AGENT_BACKEND_GSTREAMER
    CAMERA_AGENT_BACKEND_NAME="gstreamer")
  # Let the GStreamer DLLs be found when launching from the IDE, without
  # requiring a machine-specific PATH edit.
  if(GSTREAMER_RUNTIME_DIR)
    set_target_properties(camera-agent PROPERTIES
      VS_DEBUGGER_ENVIRONMENT "PATH=${GSTREAMER_RUNTIME_DIR};$ENV{PATH}")
  endif()
else()
  target_compile_definitions(camera-agent PRIVATE
    CAMERA_AGENT_BACKEND_SIM
    CAMERA_AGENT_BACKEND_NAME="sim")
endif()
