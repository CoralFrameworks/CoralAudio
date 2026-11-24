cmake_minimum_required(VERSION 3.19)
set(TARGET resampler)
set(PUBLIC_HDR_DIR include)

set(PUBLIC_HDRS
    include/resampler/HyperbolicCosineWindow.h
    include/resampler/IntegerRatio.h
    include/resampler/KaiserWindow.h
    include/resampler/LinearResampler.h
    include/resampler/MultiChannelResampler.h
    include/resampler/PolyphaseResampler.h
    include/resampler/PolyphaseResamplerMono.h
    include/resampler/PolyphaseResamplerStereo.h
    include/resampler/ResamplerDefinitions.h
    include/resampler/SincResampler.h
    include/resampler/SincResamplerStereo.h
)

set(SRCS
    src/IntegerRatio.cpp
    src/LinearResampler.cpp
    src/MultiChannelResampler.cpp
    src/PolyphaseResampler.cpp
    src/PolyphaseResamplerMono.cpp
    src/PolyphaseResamplerStereo.cpp
    src/SincResampler.cpp
    src/SincResamplerStereo.cpp
)

# ==================================================================================================
# Includes & target definition
# ==================================================================================================
# specify where our headers are
include_directories(${PUBLIC_HDR_DIR})

# we're building a library
add_library(${TARGET} STATIC ${PUBLIC_HDRS} ${SRCS})

# specify where the public headers of this library are
target_include_directories(${TARGET} PUBLIC ${PUBLIC_HDR_DIR})

# ==================================================================================================
# Dependencies
# ==================================================================================================

# ==================================================================================================
# Installation
# ==================================================================================================
install(TARGETS ${TARGET} ARCHIVE DESTINATION lib)
install(DIRECTORY ${PUBLIC_HDR_DIR}/${TARGET} DESTINATION include)
