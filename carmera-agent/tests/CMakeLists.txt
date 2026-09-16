# Unit tests. They always run against the SIM backend so they can execute
# headlessly on any machine (no camera / no GStreamer required).
set(TEST_SOURCES
  run_all.cpp
  backoff_tests.cpp
  camera_tests.cpp
  pipeline_tests.cpp
  rtsp_tests.cpp
  reconnect_tests.cpp
  config_tests.cpp
  ai_detector_tests.cpp
  metadata_tests.cpp
)

# Reuse the SIM backend sources from the app (deterministic, headless).
set(APP_SIM_SOURCES
  ${PROJECT_SOURCE_DIR}/src/config/config.cpp
  ${PROJECT_SOURCE_DIR}/src/common/stream_controller.cpp
  ${PROJECT_SOURCE_DIR}/src/common/http_client_winhttp.cpp
  ${PROJECT_SOURCE_DIR}/src/camera/camera_manager_sim.cpp
  ${PROJECT_SOURCE_DIR}/src/pipeline/video_pipeline_sim.cpp
  ${PROJECT_SOURCE_DIR}/src/rtsp/rtsp_publisher_sim.cpp
  # AI branch: stream_controller owns an AIPipeline, so it must link too.
  # Tests build without ONNX Runtime -> the null detector is used.
  ${PROJECT_SOURCE_DIR}/src/ai/ai_pipeline.cpp
  ${PROJECT_SOURCE_DIR}/src/ai/byte_tracker.cpp
  ${PROJECT_SOURCE_DIR}/src/ai/yolo_detector.cpp
  # Metadata branch: stream_controller owns a MetadataManager, so it must link.
  ${PROJECT_SOURCE_DIR}/src/metadata/metadata_encoder.cpp
  ${PROJECT_SOURCE_DIR}/src/metadata/metadata_manager.cpp
  ${PROJECT_SOURCE_DIR}/src/metadata/http_transport_winhttp.cpp
)

add_executable(camera-agent-tests ${TEST_SOURCES} ${APP_SIM_SOURCES})

target_include_directories(camera-agent-tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
if(MSVC)
  # /utf-8: sources are UTF-8; without it MSVC reads them as GBK and emits C4819.
  target_compile_options(camera-agent-tests PRIVATE /W4 /EHsc /utf-8)
else()
  target_compile_options(camera-agent-tests PRIVATE -Wall -Wextra)
endif()
target_link_libraries(camera-agent-tests PRIVATE spdlog::spdlog)
if(WIN32)
  target_link_libraries(camera-agent-tests PRIVATE winhttp)
endif()

# The AI detector tests need ONNX Runtime (if installed) and the stb_image
# header. They skip themselves cleanly when either is missing, so linking
# is best-effort.
target_include_directories(camera-agent-tests PRIVATE
  ${PROJECT_SOURCE_DIR}/third_party)
if(HAVE_ORT)
  target_include_directories(camera-agent-tests PRIVATE ${ORT_INCLUDE_DIRS})
  target_link_libraries(camera-agent-tests PRIVATE ${ORT_LIBRARIES})
  target_compile_definitions(camera-agent-tests PRIVATE CAMERA_AGENT_HAVE_ORT)
  set(_test_ort_dlls "")
  foreach(_d onnxruntime.dll onnxruntime_providers_shared.dll)
    if(EXISTS "${ORT_RUNTIME_DIR}/${_d}")
      list(APPEND _test_ort_dlls "${ORT_RUNTIME_DIR}/${_d}")
    endif()
  endforeach()
  if(_test_ort_dlls)
    add_custom_command(TARGET camera-agent-tests POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_test_ort_dlls}
              $<TARGET_FILE_DIR:camera-agent-tests>
      COMMENT "Copying ONNX Runtime DLLs next to camera-agent-tests")
  endif()
endif()

# Run tests from the project root so that "../tests/finished/bus.jpg" and
# "models/yolov8n.onnx" resolve regardless of who invoked ctest.
set_target_properties(camera-agent-tests PROPERTIES
  WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})

target_compile_definitions(camera-agent-tests PRIVATE
  CAMERA_AGENT_BACKEND_SIM
  CAMERA_AGENT_BACKEND_NAME="sim"
  CAMERA_AGENT_VERSION="0.1.0")

add_test(NAME camera-agent-tests COMMAND camera-agent-tests)
