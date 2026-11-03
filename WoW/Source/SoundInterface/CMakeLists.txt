file(GLOB_RECURSE PRIVATE_SOURCES CONFIGURE_DEPENDS "*.cpp")

add_library(SoundInterface STATIC
    ${PRIVATE_SOURCES}
)

target_include_directories(SoundInterface
    PUBLIC
        ${CMAKE_SOURCE_DIR}
)
