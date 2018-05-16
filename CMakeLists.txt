cmake_minimum_required(VERSION 3.8)

project(scene_detect)

set(CMAKE_CXX_STANDARD 14)

# Dependencies
##############
find_package(PkgConfig REQUIRED)
foreach(m avformat avcodec avfilter avutil swscale swresample)
    string(TOUPPER ${m} M)
    pkg_check_modules(${m} REQUIRED IMPORTED_TARGET lib${m})
    list(APPEND FFmpeg_LIBS PkgConfig::${m})
endforeach(m)

set(CMAKE_EXTRA_INCLUDE_FILES libavcodec/avcodec.h)

add_executable(${PROJECT_NAME}
        scene_detect.cpp
        )

target_link_libraries(${PROJECT_NAME} PRIVATE ${FFmpeg_LIBS} ${OpenCV_LIBS})