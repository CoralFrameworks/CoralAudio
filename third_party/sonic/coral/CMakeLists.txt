cmake_minimum_required(VERSION 3.19)
project(sonic)

set(TARGET sonic)
set(OUR_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)

# ==================================================================================================
# Sources and headers
# ==================================================================================================
set(PUBLIC_HDRS
    ${OUR_DIR}/sonic.h
)

set(SRCS
    ${OUR_DIR}/sonic.c
)

# ==================================================================================================
# Include and target definitions
# ==================================================================================================
include_directories(${OUR_DIR})

add_library(${TARGET} STATIC ${PUBLIC_HDRS} ${SRCS})
target_compile_options(${TARGET} PRIVATE ${OPTIMIZATION_FLAGS})
target_include_directories(${TARGET} PUBLIC ${OUR_DIR})

# ==================================================================================================
# Installation
# ==================================================================================================
# This said `install(DIRECTORY ${OUR_DIR}/sonic DESTINATION include)`, pointing at a `sonic/` *subdirectory* of the
# checkout. There is none: the library is `sonic.h` and `sonic.c` in the root of `${OUR_DIR}`. So the rule named a path
# that does not exist, and because `install(DIRECTORY)` resolves at install time rather than at configure time, the only
# symptom was an aborted install:
#
#     CMake Error at .../sonic/coral/cmake_install.cmake:36 (file):
#       file INSTALL cannot find ".../third_party/sonic/coral/../sonic": File exists.
#
# That is what `PUBLIC_HDRS` is for, and every sibling wrapper installs it this way. The consequence of the bug was not
# confined to sonic: `cmake --install` stopped there, so nothing after it in the install order -- `libs/graphics` and
# its headers among them -- was ever written.
install(FILES ${PUBLIC_HDRS} DESTINATION include)