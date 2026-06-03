if not get_option('mctp').allowed()
    subdir_done()
endif
libusb_dep = dependency('libusb-1.0')

executable(
    'mctpreactor',
    'MCTPReactorMain.cpp',
    'MCTPReactor.cpp',
    'MCTPEndpoint.cpp',
    'MCTPEndpointUtils.cpp',
    'USBRecovery.cpp',
    'MCTPCustomDevices.cpp',
    'MCTPBridgePoolDevice.cpp',
    dependencies: [default_deps, utils_dep, libusb_dep],
    install: true,
    install_dir: '/usr/libexec/dbus-sensors',
)

if get_option('mctpheartbeat').enabled()
    mctpheartbeat_cpp_args = []
    if get_option('mctp-heartbeat-restart-notification').enabled()
        mctpheartbeat_cpp_args += ['-DMCTP_HEARTBEAT_RESTART_NOTIFICATION=true']
    else
        mctpheartbeat_cpp_args += [
            '-DMCTP_HEARTBEAT_RESTART_NOTIFICATION=false',
        ]
    endif
    executable(
        'mctpheartbeat',
        'MCTPHeartBeatApp.cpp',
        cpp_args: mctpheartbeat_cpp_args,
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
