add_executable(deskbeam_client
    main.cpp
)

target_link_libraries(deskbeam_client PRIVATE deskbeam_common)
