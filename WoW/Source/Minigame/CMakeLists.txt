file(GLOB_RECURSE PRIVATE_SOURCES CONFIGURE_DEPENDS "*.cpp")

add_library(Minigame STATIC
    ${PRIVATE_SOURCES}
)

target_include_directories(Minigame
    PUBLIC
        ${CMAKE_SOURCE_DIR}
)
