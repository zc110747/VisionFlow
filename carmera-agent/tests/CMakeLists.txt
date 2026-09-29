# Unit tests. They always run against the SIM backend so they can execute
# headlessly on any machine (no camera / no GStreamer required).
set(TEST_SOURCES
  run_all.cpp
  stb_image_impl.cpp
  backoff_tests.cpp
  camera_tests.cpp
  pipeline_tests.cpp
  rtsp_tests.cpp
  reconnect_tests.cpp
  config_tests.cpp
  ai_detector_tests.cpp
  metadata_tests.cpp
  hand_analyzer_tests.cpp
  hand_detector_tests.cpp
  hand_wiring_tests.cpp
  robot_link_tests.cpp
  robot_synthetic_tests.cpp
  robot_realframe_tests.cpp
  robot_live_tests.cpp
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
  # With ONNX Runtime present the hand tests exercise a real cascade; without
  # it the factories return null detectors and every model-dependent case skips
  # itself, so the pure decode / analyzer / encoder code stays covered either way.
  ${PROJECT_SOURCE_DIR}/src/ai/ai_pipeline.cpp
  ${PROJECT_SOURCE_DIR}/src/ai/byte_tracker.cpp
  ${PROJECT_SOURCE_DIR}/src/ai/yolo_detector.cpp
  ${PROJECT_SOURCE_DIR}/src/ai/hand_decode.cpp
  ${PROJECT_SOURCE_DIR}/src/ai/hand_detector.cpp
  ${PROJECT_SOURCE_DIR}/src/ai/hand_analyzer.cpp
  # Metadata branch: stream_controller owns a MetadataManager, so it must link.
  ${PROJECT_SOURCE_DIR}/src/metadata/metadata_encoder.cpp
  ${PROJECT_SOURCE_DIR}/src/metadata/metadata_manager.cpp
  ${PROJECT_SOURCE_DIR}/src/metadata/http_transport_winhttp.cpp
  # Robot branch: stream_controller owns a RobotLink, so it must link. The tests
  # substitute a scripted transport, so no MeArmPilot backend is required.
  ${PROJECT_SOURCE_DIR}/src/robot/json_line_client_tcp.cpp
  ${PROJECT_SOURCE_DIR}/src/robot/robot_link.cpp
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
  target_link_libraries(camera-agent-tests PRIVATE winhttp ws2_32)
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
