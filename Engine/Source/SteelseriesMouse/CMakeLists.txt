file(GLOB_RECURSE PRIVATE_SOURCES CONFIGURE_DEPENDS "*.cpp")

add_library(SteelseriesMouse STATIC
    ${PRIVATE_SOURCES}
)

target_include_directories(SteelseriesMouse
    PUBLIC
        ${CMAKE_SOURCE_DIR}
)

target_link_libraries(SteelseriesMouse
    PUBLIC
        hid
        setupapi
)
