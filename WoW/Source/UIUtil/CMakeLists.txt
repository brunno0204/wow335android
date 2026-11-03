file(GLOB_RECURSE PRIVATE_SOURCES CONFIGURE_DEPENDS "*.cpp")

add_library(UIUtil STATIC
    ${PRIVATE_SOURCES}
)

target_include_directories(UIUtil
    PUBLIC
        ${CMAKE_SOURCE_DIR}
)
