# Static asset embedding: gzip -9 + xxd -i.
# Produces <target>_<name>_assets.h with the <target>_<name>_gz array and ..._gz_len.
function(core_embed_asset target name asset_file)
    set(gz_bin ${target}_${name}.gz)
    set(header ${target}_${name}_assets.h)
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${header}
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMAND gzip -9 -c ${asset_file} > ${gz_bin}
        COMMAND xxd -i ${gz_bin} > ${header}
        DEPENDS ${asset_file}
        COMMENT "Embedding gzip asset ${name} for ${target}"
    )
    add_custom_target(${target}_${name}_assets DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${header})
    add_dependencies(${target} ${target}_${name}_assets)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
endfunction()
