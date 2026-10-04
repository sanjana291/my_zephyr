if(CONFIG_TCP_SERVER)
    zephyr_library()

    zephyr_library_sources(
    tcp_server.c
    tcp_server_handshake.c
    tcp_server_packet.c
    tcp_server_frame.c
    )

endif()