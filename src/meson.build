conf_data = configuration_data()
conf_data.set10(
    'VALIDATION_UNSECURE_FEATURE',
    get_option('validate-unsecure-feature').allowed(),
)
conf_data.set10(
    'INSECURE_UNRESTRICTED_SENSOR_OVERRIDE',
    get_option('insecure-sensor-override').allowed(),
)
configure_file(
    input: 'dbus-sensor_config.h.in',
    output: 'dbus-sensor_config.h',
    configuration: conf_data,
)

thresholds_a = static_library(
    'thresholds_a',
    'Thresholds.cpp',
    dependencies: default_deps,
)

thresholds_dep = declare_dependency(
    include_directories: ['.'],
    link_with: [thresholds_a],
    dependencies: default_deps,
)

utils_a = static_library(
    'utils_a',
    ['FileHandle.cpp', 'SensorPaths.cpp', 'Utils.cpp'],
    dependencies: default_deps,
)

utils_dep = declare_dependency(
    include_directories: ['.'],
    link_with: [utils_a],
    dependencies: [sdbusplus],
)

devicemgmt_a = static_library(
    'devicemgmt_a',
    ['DeviceMgmt.cpp'],
    dependencies: default_deps,
)

devicemgmt_dep = declare_dependency(
    include_directories: ['.'],
    link_with: [devicemgmt_a],
    dependencies: default_deps,
)

pwmsensor_a = static_library(
    'pwmsensor_a',
    'PwmSensor.cpp',
    dependencies: [default_deps, thresholds_dep],
)

pwmsensor_dep = declare_dependency(
    include_directories: ['.'],
    link_with: [pwmsensor_a],
    dependencies: [default_deps, thresholds_dep],
)

systemdinterface_src = files('SystemdInterface.cpp')

gpiointerface_a = static_library(
    'gpiointerface_a',
    'GPIOInterface.cpp',
    dependencies: [default_deps, gpiodcxx],
)

gpiointerface_dep = declare_dependency(
    include_directories: ['.'],
    link_with: [gpiointerface_a],
    dependencies: default_deps,
)

entitymanagerinterface_a = static_library(
    'entitymanagerinterface_a',
    'EntityManagerInterface.cpp',
    dependencies: default_deps,
)

entitymanagerinterface_dep = declare_dependency(
    include_directories: ['.'],
    link_with: [entitymanagerinterface_a],
    dependencies: default_deps,
)

mctpasio_a = static_library(
    'mctpasio_a',
    'MctpAsioEndpoint.cpp',
    dependencies: [boost, phosphor_logging_dep, uring],
)

mctpasio_dep = declare_dependency(
    include_directories: ['.'],
    link_with: [mctpasio_a],
    dependencies: default_deps,
)

notifywatch_a = static_library(
    'notifywatch_a',
    'NotifyWatch.cpp',
    dependencies: default_deps,
)

notifywatch_dep = declare_dependency(
    include_directories: ['.'],
    link_with: [notifywatch_a],
    dependencies: default_deps,
)

src_inc = include_directories('.')

if get_option('boot-progress').allowed()
    executable(
        'bootprogress',
        'BootProgressSensor.cpp',
        dependencies: [default_deps, utils_dep],
        implicit_include_directories: false,
        include_directories: ['../include'] + peci_incdirs,
        install: true,
    )
endif

if get_option('synth').allowed()
    executable(
        'synthesizedsensor',
        'SynthesizedSensor.cpp',
        dependencies: [default_deps, thresholds_dep, utils_dep],
        install: true,
    )
endif

if get_option('plx-temp').allowed()
    executable(
        'plxtempsensor',
        'PLXTempMain.cpp',
        'PLXTempSensor.cpp',
        dependencies: [default_deps, thresholds_dep, utils_dep],
        implicit_include_directories: false,
        include_directories: '../include',
        install: true,
    )
endif

if get_option('procstatus').allowed()
    executable(
        'processorstatus',
        'ProcessorStatus.cpp',
        'ProcessorStatusMain.cpp',
        dependencies: [default_deps, gpiodcxx, utils_dep],
        implicit_include_directories: false,
        include_directories: '../include',
        install: true,
    )
endif

if get_option('nvmeu2').allowed()
    executable(
        'nvmestatus',
        'NVMeStatus.cpp',
        'NVMeMIStatus.cpp',
        'NVMeStatusMain.cpp',
        dependencies: [default_deps, i2c, utils_dep],
        implicit_include_directories: false,
        include_directories: '../include',
        install: true,
    )
endif

if get_option('ipmbstatus').allowed()
    executable(
        'ipmbstatus',
        'IpmbStatus.cpp',
        dependencies: [default_deps, thresholds_dep, utils_dep],
        implicit_include_directories: false,
        include_directories: '../include',
        install: true,
    )
endif

if get_option('system').allowed()
    executable(
        'watchdogsensor',
        'WatchdogSensor.cpp',
        'SELSensor.cpp',
        'SystemSensorMain.cpp',
        dependencies: [default_deps, utils_dep],
        implicit_include_directories: false,
        include_directories: '../include',
        install: true,
    )
endif

if get_option('psuredundancy').allowed()
    executable(
        'psuredundancy',
        'PSURedundancy.cpp',
        'PSURedundancyMain.cpp',
        dependencies: [default_deps, utils_dep],
        implicit_include_directories: false,
        include_directories: '../include',
        install: true,
    )
endif

if get_option('satellite').enabled()
    executable(
        'satellitesensor',
        'SatelliteSensor.cpp',
        dependencies: [default_deps, i2c, thresholds_dep, utils_dep],
        implicit_include_directories: false,
        include_directories: include_directories(['../include', '../src']),
        install: true,
    )
endif

if get_option('leak-detect').allowed()
    executable(
        'leakdetectsensor',
        'LeakDetectSensor.cpp',
        'LeakDetectSensorMain.cpp',
        dependencies: [
            default_deps,
            gpiodcxx,
            thresholds_dep,
            utils_dep,
            devicemgmt_dep,
        ],
        install: true,
    )
    subdir('external')
endif

if get_option('discrete-leak-detect').allowed()
    executable(
        'discreteleakdetectsensor',
        'DiscreteLeakDetectSensor.cpp',
        'DiscreteLeakDetectSensorMain.cpp',
        dependencies: [default_deps, utils_dep],
        include_directories: '../include',
        install: true,
    )
endif

if get_option('presence-detect').enabled()
    executable(
        'presence-detect',
        'GPIOPresenceSensor.cpp',
        'GPIOPresenceSensorMain.cpp',
        dependencies: [
            sdbusplus,
            gpiodcxx,
            uring,
            phosphor_logging_dep,
            i2c,
            utils_dep,
        ],
        implicit_include_directories: false,
        include_directories: '../include',
        install: true,
    )
endif

if get_option('write-protect').enabled()
    executable(
        'writeprotectsensor',
        'WriteProtectSensor.cpp',
        'WriteProtectSensorMain.cpp',
        cpp_args: [
            '-DWRITEPROTECT_PARENT_CHASSIS_ID="' + get_option(
                'writeprotect-parent-chassis-id',
            ) + '"',
        ],
        dependencies: [sdbusplus, gpiodcxx, uring, phosphor_logging_dep],
        implicit_include_directories: false,
        include_directories: '../include',
        install: true,
    )
endif

if get_option('port-ctrl').allowed()
    executable(
        'portctrl',
        'PortCtrl.cpp',
        dependencies: [
            sdbusplus,
            boost,
            uring,
            phosphor_logging_dep,
            gpiodcxx,
            nlohmann_json_dep,
        ],
        implicit_include_directories: false,
        install: true,
    )
endif

if get_option('nvidia-fw-write-protect').enabled()
    subdir('nvidia-fw-write-protect')
endif

if get_option('debug-serial-ctrl').allowed()
    executable(
        'debugserialctrl',
        'DebugSerialCtrl.cpp',
        dependencies: [sdbusplus, boost, uring, phosphor_logging_dep],
        implicit_include_directories: false,
        install: true,
    )
endif

if get_option('openocd-port-forward').allowed()
    executable(
        'openocdportforward',
        ['OpenOCDPortForwardMain.cpp', 'OpenOCDPortForwardPorts.cpp'],
        dependencies: [default_deps, sdbusplus],
        install: true,
    )
endif

subdir('adc')
subdir('cable-monitor')
subdir('exit-air')
subdir('external')
subdir('fan')
subdir('hwmon-temp')
subdir('intel-cpu')
subdir('intrusion')
subdir('ipmb')
subdir('leakdetector')
subdir('mctp')
subdir('mcu')
subdir('nvidia-gpu')
subdir('nvme')
subdir('psu')
subdir('smbpbi')
subdir('tests')
