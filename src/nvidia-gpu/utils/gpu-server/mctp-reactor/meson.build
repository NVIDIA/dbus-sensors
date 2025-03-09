executable(
    'gpuserver-mctp-reactor',
    [
        'mctp_reactor.cpp',
        'mctp_discovery.cpp'
    ],
    dependencies: [
        dependency('phosphor-logging'),
        dependency('sdbusplus'),
        gpuserver_lib_dep
    ],
    include_directories: include_directories([
        '..',
        '../lib',
    ]),
    install: true,
    install_dir: get_option('bindir')
)
