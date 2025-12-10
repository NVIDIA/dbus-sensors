src_inc = include_directories('..')

intrusion_cpp_args = []
if get_option('intrusion-dbus-log').enabled()
    intrusion_cpp_args += ['-DINTRUSION_DBUS_LOG']
endif

executable(
    'intrusionsensor',
    'ChassisIntrusionSensor.cpp',
    'IntrusionSensorMain.cpp',
    dependencies: [default_deps, gpiodcxx, i2c, utils_dep],
    include_directories: src_inc,
    cpp_args: intrusion_cpp_args,
    install: true,
    install_dir: '/usr/libexec/dbus-sensors',
)
