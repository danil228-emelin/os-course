add_executable(
    vtsh
    main.c
)

add_executable(
    shell
    main.c
)

target_include_directories(
    vtsh
    PUBLIC
    .
)

target_link_libraries(
    vtsh
    PRIVATE
    libvtsh
)


target_link_libraries(
    shell
    PRIVATE
    libvtsh
)