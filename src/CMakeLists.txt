add_subdirectory(core ${PROJECT_BINARY_DIR}/core)
add_subdirectory(data_structure ${PROJECT_BINARY_DIR}/data_structure)
add_subdirectory(hotkey ${PROJECT_BINARY_DIR}/hotkey)
add_subdirectory(protocol ${PROJECT_BINARY_DIR}/protocol)
add_subdirectory(storage ${PROJECT_BINARY_DIR}/storage)
add_subdirectory(time ${PROJECT_BINARY_DIR}/time)
add_subdirectory(util ${PROJECT_BINARY_DIR}/util)
add_subdirectory(datapool ${PROJECT_BINARY_DIR}/datapool)

if (HAVE_RUST)
    add_subdirectory(rust-util ${PROJECT_BINARY_DIR}/rust-util)
    add_subdirectory(rustcore ${PROJECT_BINARY_DIR}/rustcore)
endif()

# executables
add_custom_target(service)
add_subdirectory(client ${PROJECT_BINARY_DIR}/client)
add_subdirectory(server ${PROJECT_BINARY_DIR}/server)
