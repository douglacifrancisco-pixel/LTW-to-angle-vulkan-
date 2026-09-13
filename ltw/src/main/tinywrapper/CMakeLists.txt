cmake_minimum_required(VERSION 3.10.0)
project(LTW VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)

add_compile_options(-fvisibility=default)

add_library(ltw SHARED
    basevertex.c
    blending.c
    egl.c
    framebuffer.c
    env.c
    glformats.c
    license_notice.c
    main.c
    multidraw.c
    of_buffer_copier.c
    proc.c
    query.c
    shader_wrapper.c
    string_utils.c
    stubs.c
    swizzle.c
    vertexattrib.c
)

include_directories("${CMAKE_SOURCE_DIR}")
add_subdirectory(unordered_map)
add_subdirectory(vgpu_shaderconv)
add_subdirectory(glsl_optimizer)

set(LTW_EGL_KEEP_SYMBOLS
    eglGetDisplay
    eglInitialize
    eglTerminate
    eglChooseConfig
    eglGetError
    eglQueryString
    eglCreateWindowSurface
    eglCreatePbufferSurface
    eglDestroySurface
    eglSwapBuffers
    eglGetCurrentContext
    eglGetCurrentDisplay
    eglGetCurrentSurface
)

set(LTW_EGL_KEEP_LINK_OPTIONS)
foreach(symbol IN LISTS LTW_EGL_KEEP_SYMBOLS)
    list(APPEND LTW_EGL_KEEP_LINK_OPTIONS "-Wl,--undefined=${symbol}")
endforeach()

target_link_options(ltw PUBLIC
    ${LTW_EGL_KEEP_LINK_OPTIONS}
    -Wl,--no-gc-sections
)

target_link_libraries(ltw unordered_map vgpu_shaderconv glsl_optimizer)
if(ANDROID)
    target_link_libraries(ltw log)
endif()
