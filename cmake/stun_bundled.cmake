MESSAGE(STATUS "Enable building of the bundled libstun")

set (STUN_DIR third/stun)
set (STUN_SRC_DIR ${PROJECT_SOURCE_DIR}/${STUN_DIR})
set (STUN_BIN_DIR ${PROJECT_BINARY_DIR}/${STUN_DIR})
set (STUN_BUNDLED_LIB ${STUN_BIN_DIR}/build/libstun.a)

add_custom_target(libstun ALL DEPENDS ${STUN_BUNDLED_LIB})

file(MAKE_DIRECTORY ${STUN_BIN_DIR}/build)
add_custom_command(OUTPUT ${STUN_BUNDLED_LIB}
    PRE_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${STUN_SRC_DIR} ${STUN_BIN_DIR}
    COMMAND cmake ..
    COMMAND $(MAKE)
    WORKING_DIRECTORY ${STUN_BIN_DIR}/build)

set(STUN_BUNDLED_INCLUDE_DIRS ${STUN_BIN_DIR})

add_library(STUN_bundled STATIC IMPORTED)
set_property(TARGET STUN_bundled PROPERTY IMPORTED_LOCATION ${STUN_BUNDLED_LIB})
set(STUN_BUNDLED_LIBS ${STUN_BUNDLED_LIB})

install_files(${STUN_BIN_DIR}/stun.h DESTINATION /usr/include/sems/stun/stun.h)
