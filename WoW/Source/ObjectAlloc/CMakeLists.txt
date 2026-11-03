file(GLOB_RECURSE PRIVATE_SOURCES CONFIGURE_DEPENDS "*.cpp")

add_library(ObjectAlloc STATIC
    ${PRIVATE_SOURCES}
)

target_include_directories(ObjectAlloc
    PUBLIC
        ${CMAKE_SOURCE_DIR}
)
