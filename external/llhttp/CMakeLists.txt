# llhttp (MIT) — Node.js HTTP/1.1 parser v9.2.1
# Integrated as static library. Original project CMakeLists replaced.

add_library(llhttp STATIC
    src/llhttp.c
    src/http.c
    src/api.c
)

target_include_directories(llhttp PUBLIC include)
target_compile_features(llhttp PUBLIC c_std_99)

# Suppress third-party warnings.
if(MSVC)
  target_compile_options(llhttp PRIVATE /W3)
endif()
