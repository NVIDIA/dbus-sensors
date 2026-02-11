executable(
    'mctpreactor',
    'MCTPReactorMain.cpp',
    'MCTPReactor.cpp',
    'MCTPEndpoint.cpp',
    'MCTPEndpointUtils.cpp',
    'MCTPCustomDevices.cpp',
    dependencies: [default_deps, utils_dep],
    install: true,
    install_dir: '/usr/libexec/dbus-sensors',
)
if get_option('mctpheartbeat').enabled()
    executable(
        'mctpheartbeat',
        'MCTPHeartBeatApp.cpp',
        dependencies: [default_deps, utils_dep],
        install: true,
    )
    install_data(
        'reboot-wrapper.sh',
        install_dir: get_option('sbindir'),
        rename: 'reboot-wrapper',
        install_mode: 'rwxr-xr-x',
    )
endif

if get_option('tests').allowed()
    subdir('tests')
endif
