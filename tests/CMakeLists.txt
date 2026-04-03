if(WIN32)
    add_executable(capture_test
        capture_test.cpp
        ${CMAKE_SOURCE_DIR}/src/host/capture/dxgi_capture.cpp
    )
    target_link_libraries(capture_test PRIVATE deskbeam_common d3d11 dxgi)
    target_include_directories(capture_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME capture_test COMMAND capture_test)

    add_executable(encode_test
        encode_test.cpp
        ${CMAKE_SOURCE_DIR}/src/host/capture/dxgi_capture.cpp
        ${CMAKE_SOURCE_DIR}/src/host/encode/amf_encoder.cpp
    )
    target_link_libraries(encode_test PRIVATE deskbeam_common d3d11 dxgi)
    target_include_directories(encode_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/third_party/amf
    )
    add_test(NAME encode_test COMMAND encode_test)

    add_executable(sdr_white_test sdr_white_test.cpp)
    target_include_directories(sdr_white_test PRIVATE ${CMAKE_SOURCE_DIR}/src)

    add_executable(capture_diag
        capture_diag.cpp
        ${CMAKE_SOURCE_DIR}/src/host/capture/dxgi_capture.cpp
    )
    target_link_libraries(capture_diag PRIVATE deskbeam_common d3d11 dxgi)
    target_include_directories(capture_diag PRIVATE ${CMAKE_SOURCE_DIR}/src)
    add_executable(fragment_test fragment_test.cpp)
    target_link_libraries(fragment_test PRIVATE deskbeam_common)
    target_include_directories(fragment_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME fragment_test COMMAND fragment_test)

    add_executable(transport_test
        transport_test.cpp
        ${CMAKE_SOURCE_DIR}/src/host/capture/dxgi_capture.cpp
        ${CMAKE_SOURCE_DIR}/src/host/encode/amf_encoder.cpp
        ${CMAKE_SOURCE_DIR}/src/host/session/video_sender.cpp
        ${CMAKE_SOURCE_DIR}/src/client/net/video_receiver.cpp
    )
    target_link_libraries(transport_test PRIVATE deskbeam_common d3d11 dxgi)
    target_include_directories(transport_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/third_party/amf
    )
    add_test(NAME transport_test COMMAND transport_test)

    add_executable(decode_test
        decode_test.cpp
        ${CMAKE_SOURCE_DIR}/src/client/decode/mf_decoder.cpp
    )
    target_link_libraries(decode_test PRIVATE deskbeam_common d3d11)
    target_include_directories(decode_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME decode_test COMMAND decode_test)
endif()
