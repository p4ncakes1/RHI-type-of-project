cmake_minimum_required(VERSION 3.20)

project(TestApp LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_definitions(_CRT_SECURE_NO_WARNINGS)

file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
    src/*.c
)

add_executable(${PROJECT_NAME} ${SRC_FILES})

target_include_directories(${PROJECT_NAME}
    PUBLIC include
    PRIVATE internal
)

set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

add_subdirectory(thirdparty/glfw)
add_subdirectory(thirdparty/cglm)

find_package(Vulkan REQUIRED)

if (MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4)
else()
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wpedantic)
endif()

if (WIN32)
    set(DIRECTX_LIBS
        d3d11
        dxgi
        d3dcompiler
        dxguid
    )
    set(PLATFORM_LIBS
        opengl32
        ${DIRECTX_LIBS}
    )
elseif(UNIX)
    set(PLATFORM_LIBS
        GL
        X11
        pthread
        dl
    )
endif()

target_link_libraries(${PROJECT_NAME}
    PRIVATE
        glfw
        cglm
        Vulkan::Vulkan
        ${PLATFORM_LIBS}
)