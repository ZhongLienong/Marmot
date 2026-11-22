add_executable(marmotvm src/Wasm/Bindings.cpp)
target_link_libraries(marmotvm PRIVATE MarmotRuntime)
if(MARMOT_WASM_HOST_SOURCE)
    target_sources(marmotvm PRIVATE "${MARMOT_WASM_HOST_SOURCE}")
    target_link_libraries(marmotvm PRIVATE "${MARMOT_WASM_HOST_LIBRARY}")
endif()
set(MARMOT_WASM_TARGETS ${MARMOT_LIBRARY_TARGETS} marmotvm)
foreach(target_name IN LISTS MARMOT_WASM_TARGETS)
    target_compile_definitions(${target_name} PRIVATE MARMOT_WASM=1)
    target_compile_options(${target_name} PRIVATE
        -fexceptions
        -Wno-deprecated-literal-operator
        $<$<AND:$<CONFIG:Release>,$<BOOL:${MARMOT_ENABLE_LTO}>>:-flto>
        -sMEMORY64=1
    )
endforeach()
target_link_options(marmotvm PRIVATE
    --bind
    --no-entry
    -sALLOW_MEMORY_GROWTH=1
    -sMODULARIZE=1
    -sEXPORT_NAME=createMarmotvmModule
    -sENVIRONMENT=web,worker
    -sSTACK_SIZE=5MB
    -sINITIAL_MEMORY=32MB
    -sMAXIMUM_MEMORY=2GB
    -sNO_DISABLE_EXCEPTION_CATCHING
    -sFORCE_FILESYSTEM=1
    -sEXPORTED_RUNTIME_METHODS=FS,FS_createPath,FS_createDataFile
    $<$<AND:$<CONFIG:Release>,$<BOOL:${MARMOT_ENABLE_LTO}>>:-flto>
    -sMEMORY64=1
)
set_target_properties(marmotvm PROPERTIES SUFFIX ".js")
