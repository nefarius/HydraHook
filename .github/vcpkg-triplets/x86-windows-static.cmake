set(VCPKG_TARGET_ARCHITECTURE x86)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)

# CI builds Release and Release_LIB only.
set(VCPKG_BUILD_TYPE release)

# HydraHook-OpenCV links core, imgproc, flann, features2d, calib3d, and video.
# The other modules below are part of the upstream Windows build and are unused here.
# An empty CPU_DISPATCH keeps the baseline SIMD kernels (SSE2 on x86) and skips the
# extra SSE4/AVX copies.
if(PORT STREQUAL "opencv4")
    list(APPEND VCPKG_CMAKE_CONFIGURE_OPTIONS
        -DBUILD_opencv_imgcodecs=OFF
        -DBUILD_opencv_ml=OFF
        -DBUILD_opencv_objdetect=OFF
        -DBUILD_opencv_photo=OFF
        -DBUILD_opencv_stitching=OFF
        -DBUILD_opencv_videoio=OFF
        "-DCPU_DISPATCH:STRING="
    )
endif()
