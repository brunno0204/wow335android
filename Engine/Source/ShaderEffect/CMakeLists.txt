file(GLOB_RECURSE PRIVATE_SOURCES CONFIGURE_DEPENDS "*.cpp")

add_library(ShaderEffect STATIC
    ${PRIVATE_SOURCES}
)

target_include_directories(ShaderEffect
    PUBLIC
        ${CMAKE_SOURCE_DIR}
)
