executable(
    'mctpreactor',
    'MCTPReactorMain.cpp',
    'MCTPReactor.cpp',
    'MCTPEndpoint.cpp',
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
endif
