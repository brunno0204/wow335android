file(GLOB_RECURSE PRIVATE_SOURCES CONFIGURE_DEPENDS "*.cpp")

add_library(ComSat STATIC
    ${PRIVATE_SOURCES}
)

target_include_directories(ComSat
    PUBLIC
        ${CMAKE_SOURCE_DIR}
)
