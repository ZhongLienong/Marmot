option(MIDORI_WASM64 "Build WebAssembly with memory64 (wasm64)" ON)
set(MIDORI_WASM_FLAGS)
if(MIDORI_WASM64)
    list(APPEND MIDORI_WASM_FLAGS -sMEMORY64=1)
endif()
add_executable(marmotc src/Wasm/Bindings.cpp)
target_link_libraries(marmotc PRIVATE MarmotCompiler)
foreach(target_name IN LISTS MIDORI_LIBRARY_TARGETS ITEMS marmotc)
    target_compile_definitions(${target_name} PRIVATE MIDORI_WASM=1)
    if(MIDORI_WASM64)
        target_compile_definitions(${target_name} PRIVATE MIDORI_WASM64=1)
    endif()
    target_compile_options(${target_name} PRIVATE
        -fexceptions
        -Wno-deprecated-literal-operator
        $<$<AND:$<CONFIG:Release>,$<BOOL:${MIDORI_ENABLE_LTO}>>:-flto>
        ${MIDORI_WASM_FLAGS}
    )
endforeach()
target_link_options(marmotc PRIVATE
    --bind
    --no-entry
    -sALLOW_MEMORY_GROWTH=1
    -sMODULARIZE=1
    -sEXPORT_NAME=createMarmotcModule
    -sENVIRONMENT=web
    -sSTACK_SIZE=5MB
    -sINITIAL_MEMORY=32MB
    -sMAXIMUM_MEMORY=2GB
    -sNO_DISABLE_EXCEPTION_CATCHING
    -sFORCE_FILESYSTEM=1
    -sEXPORTED_RUNTIME_METHODS=FS,FS_createPath,FS_createDataFile
    $<$<AND:$<CONFIG:Release>,$<BOOL:${MIDORI_ENABLE_LTO}>>:-flto>
    ${MIDORI_WASM_FLAGS}
)
set_target_properties(marmotc PROPERTIES SUFFIX ".js")
