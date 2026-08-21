# SPDX-FileCopyrightText: 2026 Andrii Myronov
# SPDX-License-Identifier: AGPL-3.0-or-later

add_executable(vivora_host
    main.cpp
)

target_link_libraries(vivora_host PRIVATE vivora_common)

if(WIN32)
    target_sources(vivora_host PRIVATE
        capture/dxgi_capture.cpp
        encode/amf_encoder.cpp
        session/video_sender.cpp
        session/paced_sender.cpp
        session/host_session.cpp
    )
    target_include_directories(vivora_host PRIVATE
        ${CMAKE_SOURCE_DIR}/third_party/amf
    )
    target_link_libraries(vivora_host PRIVATE
        d3d11
        dxgi
    )
endif()
