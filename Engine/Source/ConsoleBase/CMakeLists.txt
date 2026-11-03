file(GLOB_RECURSE PRIVATE_SOURCES CONFIGURE_DEPENDS "*.cpp")

add_library(ConsoleBase STATIC
    ${PRIVATE_SOURCES}
)

target_include_directories(ConsoleBase
    PUBLIC
        ${CMAKE_SOURCE_DIR}
)
