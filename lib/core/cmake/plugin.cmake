# Turns a target into a -nostdlib plugin: no NEEDED, no heap imports
function(core_plugin target)
    set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_compile_options(${target} PRIVATE -fno-stack-protector)
    target_link_options(${target} PRIVATE -nostdlib)
endfunction()
