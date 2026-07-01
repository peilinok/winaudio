# Warning + conformance flags for OUR targets (not third-party).
# /std:c++17 comes from the global CMAKE_CXX_STANDARD; /W3 is absent because
# CMP0092 is NEW (cmake_minimum_required 3.21), so /W4 adds cleanly (no D9025).
function(wa_set_project_warnings target)
    target_compile_options(${target} PRIVATE
        /W4            # warning level 4
        /permissive-   # conformance mode
        /utf-8         # UTF-8 source + execution charset
        /sdl           # additional security checks
    )
endfunction()
