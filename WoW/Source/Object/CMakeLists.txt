file(GLOB_RECURSE PRIVATE_SOURCES CONFIGURE_DEPENDS "*.cpp")

add_library(Object STATIC
    ${PRIVATE_SOURCES}
)

target_include_directories(Object
    PUBLIC
        ${CMAKE_SOURCE_DIR}
)
