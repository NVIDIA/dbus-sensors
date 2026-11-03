src_inc = include_directories('..')

power_mode_store = get_option('lpu-power-mode-store')
if not power_mode_store.startswith('/')
    error(
        'lpu-power-mode-store must be an absolute path on storage that survives resets on this platform',
    )
endif

boost_context = meson.get_compiler('cpp').find_library('boost_context')

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
    cpp_args: ['-DLPU_POWER_MODE_STORE="' + power_mode_store + '"'],
    dependencies: [
        boost_context,
        default_deps,
        i2c,
        thresholds_dep,
        threads,
        utils_dep,
    ],
    implicit_include_directories: false,
    include_directories: [include_directories('../../include'), src_inc],
    install: true,
    install_dir: '/usr/libexec/dbus-sensors',
)
