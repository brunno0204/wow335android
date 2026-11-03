file(GLOB PRIVATE_SOURCES CONFIGURE_DEPENDS "src/*.c" "src/*.cpp")

set_source_files_properties(${PRIVATE_SOURCES} PROPERTIES LANGUAGE CXX)

add_library(lua STATIC
    ${PRIVATE_SOURCES}
)

target_include_directories(lua
    PUBLIC
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)
