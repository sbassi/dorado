set(TORCH_VERSION 1.12.1)
set(CUDNN_LIBRARY_PATH ${DORADO_3RD_PARTY}/fake_cudnn)
set(CUDNN_INCLUDE_PATH ${DORADO_3RD_PARTY}/fake_cudnn)

set(CMAKE_CUDA_ARCHITECTURES 70 72 75 80 86)
if(DEFINED CUDAToolkit_VERSION_MAJOR)
    if(${CUDAToolkit_VERSION_MAJOR} VERSION_LESS 11)
        set(CMAKE_CUDA_ARCHITECTURES 62 70 72)
    endif()
endif()

# TODO: sort out download location for ARM cuda 10.2 build

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if(DORADO_USING_OLD_CPP_ABI)
        set(TORCH_URL https://download.pytorch.org/libtorch/cu113/libtorch-shared-with-deps-${TORCH_VERSION}%2Bcu113.zip)
        set(TORCH_LIB "${DORADO_3RD_PARTY}/torch-no-cxx11-abi-${TORCH_VERSION}-${CMAKE_SYSTEM_NAME}/libtorch")
    else()
        set(TORCH_URL https://download.pytorch.org/libtorch/cu113/libtorch-cxx11-abi-shared-with-deps-${TORCH_VERSION}%2Bcu113.zip)
        set(TORCH_LIB "${DORADO_3RD_PARTY}/torch-${TORCH_VERSION}-${CMAKE_SYSTEM_NAME}/libtorch")
    endif()
elseif(APPLE)
    if (CMAKE_SYSTEM_NAME STREQUAL "iOS")
        set(TORCH_URL https://nanoporetech.box.com/shared/static/nzdq2wk45pzbwi2zex92j28dt3s5k9vt.tgz)
        set(TORCH_LIB "${DORADO_3RD_PARTY}/torch-${TORCH_VERSION}-${CMAKE_SYSTEM_NAME}")
    else()
        set(TORCH_URL https://files.pythonhosted.org/packages/c8/74/fd7d90bb7c589a695417e6922149dc3eb29fe0c9a97b6fb28ae851f1c19f/torch-${TORCH_VERSION}-cp39-none-macosx_11_0_arm64.whl)
        set(TORCH_LIB "${DORADO_3RD_PARTY}/torch-${TORCH_VERSION}-${CMAKE_SYSTEM_NAME}/torch")
    endif()
elseif(WIN32)
    set(TORCH_URL https://download.pytorch.org/libtorch/cu113/libtorch-win-shared-with-deps-${TORCH_VERSION}%2Bcu113.zip)
    set(TORCH_LIB "${DORADO_3RD_PARTY}/torch-${TORCH_VERSION}-${CMAKE_SYSTEM_NAME}/libtorch")
    add_compile_options(
        # Note we need to use the generator expression to avoid setting this for CUDA.
        $<$<COMPILE_LANGUAGE:CXX>:/wd4624> # from libtorch: destructor was implicitly defined as deleted 
    )
endif()

if(DEFINED DORADO_LIBTORCH_DIR)
    # Use the existing libtorch we have been pointed at
    list(APPEND CMAKE_PREFIX_PATH ${DORADO_LIBTORCH_DIR})
    message(STATUS "Using existing libtorch at ${DORADO_LIBTORCH_DIR}")
    set(TORCH_LIB ${DORADO_LIBTORCH_DIR})
else()
    # Get libtorch (if we don't already have it)
    if(DORADO_USING_OLD_CPP_ABI AND NOT WIN32 AND NOT APPLE)
        download_and_extract(${TORCH_URL} torch-no-cxx11-abi-${TORCH_VERSION}-${CMAKE_SYSTEM_NAME})
    else()
        download_and_extract(${TORCH_URL} torch-${TORCH_VERSION}-${CMAKE_SYSTEM_NAME})
    endif()
    list(APPEND CMAKE_PREFIX_PATH "${TORCH_LIB}")
endif()

find_package(Torch REQUIRED)

if(WIN32 AND DEFINED MKL_ROOT)
    link_directories(${MKL_ROOT}/lib/intel64)
endif()
