file(GLOB_RECURSE PRIVATE_SOURCES CONFIGURE_DEPENDS "*.cpp")

add_library(Base STATIC
    ${PRIVATE_SOURCES}
)

target_include_directories(Base
    PUBLIC
        ${CMAKE_SOURCE_DIR}
)
