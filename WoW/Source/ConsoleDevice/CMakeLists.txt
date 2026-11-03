file(GLOB_RECURSE PRIVATE_SOURCES CONFIGURE_DEPENDS "*.cpp")

add_library(ConsoleDevice STATIC
    ${PRIVATE_SOURCES}
)

target_include_directories(ConsoleDevice
    PUBLIC
        ${CMAKE_SOURCE_DIR}
)
