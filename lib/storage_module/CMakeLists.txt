if(CONFIG_STORAGE_MODULE)
    zephyr_library()
    zephyr_library_sources(
        storage_module.c
    )
endif()