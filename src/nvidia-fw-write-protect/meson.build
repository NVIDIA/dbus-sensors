phosphor_dbus_interfaces_dep = dependency('phosphor-dbus-interfaces')

executable(
    'nvidia-fw-write-protect',
    'main.cpp',
    'DomainManager.cpp',
    'Domain.cpp',
    'GroupConfig.cpp',
    'GpioInputConfig.cpp',
    'ReactiveGraph.cpp',
    'Protectors/DbusProxy.cpp',
    'Protectors/Gpio.cpp',
    dependencies: [
        default_deps,
        utils_dep,
        phosphor_dbus_interfaces_dep,
        entitymanagerinterface_dep,
        gpiodcxx,
    ],
    install: true,
    install_dir: '/usr/libexec/dbus-sensors/',
)
