file(GLOB_RECURSE PRIVATE_SOURCES CONFIGURE_DEPENDS "*.cpp")

add_library(Magic STATIC
    ${PRIVATE_SOURCES}
)

target_include_directories(Magic
    PUBLIC
        ${CMAKE_SOURCE_DIR}
)
