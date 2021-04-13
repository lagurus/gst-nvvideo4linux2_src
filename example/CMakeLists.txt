set(DEMO_NAME gst_opencv_motion)
project(${DEMO_NAME})

#unset(USE_MY_LIB CACHE)

cmake_minimum_required(VERSION 0.8)

set(CMAKE_BUILD_TYPE Debug)
set(CMAKE_CXX_STANDARD 11)

find_package(OpenCV REQUIRED)
find_package(PkgConfig REQUIRED)

#using pkg-config to getting Gstreamer
pkg_check_modules(GSTREAMER REQUIRED gstreamer-1.0 gstreamer-app-1.0)


# ---------------------------------------------------------------------------------------------------

#including GStreamer header files directory
include_directories(
        ${GLIB_INCLUDE_DIRS}
        ${GSTREAMER_INCLUDE_DIRS}
)

#linking GStreamer library directory
link_directories(
        ${GLIB_LIBRARY_DIRS}
        ${GSTREAMER_LIBRARY_DIRS}
)

# ---------------------------------------------------------------------------------------------------




add_executable(${DEMO_NAME} gst_opencv_motion.cpp  )

target_link_libraries(${DEMO_NAME} ${OpenCV_LIBS} ${GSTREAMER_LIBRARIES} )

