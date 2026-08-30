cmake_minimum_required(VERSION 3.20)
project(dlss5_opengl_bridge LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR
        "dlss5-opengl-bridge targets Windows (MSVC): it uses D3D12, WGL, the "
        "Win32 external-memory OpenGL extensions and SEH. Configure on Windows.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# No external dependencies. The OpenGL entry points are resolved at runtime
# through wglGetProcAddress, the NGX interfaces are declared inline in ngx.h,
# the ReShade add-on API is reached through GetProcAddress, and d3d12.h and
# dxgi1_6.h come from the Windows SDK with their functions loaded by name.
add_library(dlss5-opengl-bridge SHARED
    src/dlss5-opengl-bridge.cpp
    src/version.rc
)

set_target_properties(dlss5-opengl-bridge PROPERTIES
    OUTPUT_NAME "dlss5-opengl-bridge"
    PREFIX ""
    SUFFIX ".addon64"
)

target_compile_definitions(dlss5-opengl-bridge PRIVATE UNICODE _UNICODE)
target_link_libraries(dlss5-opengl-bridge PRIVATE kernel32 user32 gdi32 advapi32)

if(MSVC)
    target_compile_options(dlss5-opengl-bridge PRIVATE /W3 /MP)
    set_property(TARGET dlss5-opengl-bridge PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

install(TARGETS dlss5-opengl-bridge RUNTIME DESTINATION .)
