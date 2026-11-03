file(GLOB_RECURSE PRIVATE_SOURCES CONFIGURE_DEPENDS "*.cpp")

add_library(BLPFile STATIC
    ${PRIVATE_SOURCES}
)

target_include_directories(BLPFile
    PUBLIC
        ${CMAKE_SOURCE_DIR}
)
