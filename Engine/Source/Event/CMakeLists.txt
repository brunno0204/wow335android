file(GLOB_RECURSE PRIVATE_SOURCES CONFIGURE_DEPENDS "*.cpp")

add_library(Event STATIC
    ${PRIVATE_SOURCES}
)

target_include_directories(Event
    PUBLIC
        ${CMAKE_SOURCE_DIR}
)
