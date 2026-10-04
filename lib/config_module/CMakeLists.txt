if(CONFIG_CONFIG_MODULE)
    zephyr_library()

    zephyr_library_sources(
        config_module.c
        config_module_json.c
        config_module_validate.c
    )

endif()