file(GLOB_RECURSE PRIVATE_SOURCES CONFIGURE_DEPENDS "*.cpp")

add_library(Battlenet STATIC
    ${PRIVATE_SOURCES}
)

target_include_directories(Battlenet
    PUBLIC
        ${CMAKE_SOURCE_DIR}
)
