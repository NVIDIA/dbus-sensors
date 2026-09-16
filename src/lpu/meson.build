src_inc = include_directories('..')

executable(
    'lputelemetry',
    files(
        'LpuCommon.cpp',
        'LpuMain.cpp',
        'LpuOnBootFields.cpp',
        'LpuPowerModeService.cpp',
        'LpuProcessorMetrics.cpp',
        'LpuVrSensors.cpp',
    ),
    dependencies: [default_deps, i2c, thresholds_dep, utils_dep],
    implicit_include_directories: false,
    include_directories: [include_directories('../../include'), src_inc],
    install: true,
    install_dir: '/usr/libexec/dbus-sensors',
)
