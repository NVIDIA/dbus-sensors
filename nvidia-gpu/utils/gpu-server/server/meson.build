gpuserverd_sources = [
    'gpuserverd.cpp',
    'manager.cpp',
    'client_connection.cpp',
    'mctp_endpoint_manager.cpp',
    'utils.cpp',
    'request_timeout_tracker.cpp',
    'socket_handler.cpp',
]

sdeventplus = dependency('sdeventplus')

gpuserverd_dependencies = [
    phosphor_logging_dep,
    sdbusplus,
    sdeventplus,
    gpuserver_lib_dep,
]

gpuserverd_includes = include_directories(
    '.',
    '..',
    '../lib'
)

executable(
    'gpuserverd',
    gpuserverd_sources,
    dependencies: gpuserverd_dependencies,
    include_directories: gpuserverd_includes,
    install: true,
    install_dir: get_option('bindir')
)
