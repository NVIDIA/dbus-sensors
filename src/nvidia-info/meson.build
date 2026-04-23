nvidia_info_sources = files(
    'NvidiaInfo.cpp',
    'NvidiaInfoMain.cpp',
    'NvidiaInfoSchema.cpp',
)

nvidia_info_include_dir = include_directories('.', is_system: true)
sensor_include_dir = include_directories('../..')

executable(
    'nvidiainfo',
    nvidia_info_sources,
    cpp_args: ['-DJSON_IMPLICIT_CONVERSIONS=0'],
    implicit_include_directories: false,
    include_directories: [nvidia_info_include_dir, sensor_include_dir],
    dependencies: [default_deps, nlohmann_json_dep],
    install: true,
    install_dir: '/usr/libexec/dbus-sensors',
)
