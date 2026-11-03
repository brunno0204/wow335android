file(GLOB_RECURSE PRIVATE_SOURCES CONFIGURE_DEPENDS "*.cpp")

add_library(FFX STATIC
    ${PRIVATE_SOURCES}
)

target_include_directories(FFX
    PUBLIC
        ${CMAKE_SOURCE_DIR}
)
