idf_component_register(SRCS "bootloader_start.c"
                    REQUIRES bootloader bootloader_support)

# Force the hook component into the bootloader image. Its hook symbols are
# otherwise weak and would be discarded from the static archive.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u bootloader_hooks_include")

# Use the default linker scripts
idf_build_get_property(scripts BOOTLOADER_LINKER_SCRIPT)
target_linker_script(${COMPONENT_LIB} INTERFACE "${scripts}")
