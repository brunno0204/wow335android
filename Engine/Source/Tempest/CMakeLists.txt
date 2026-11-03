file(GLOB_RECURSE PRIVATE_SOURCES CONFIGURE_DEPENDS "*.cpp")

add_library(Tempest STATIC
    ${PRIVATE_SOURCES}
)

target_include_directories(Tempest
    PUBLIC
        ${CMAKE_SOURCE_DIR}
)
