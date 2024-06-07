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
    link_with: [thresholds_a],
    dependencies: default_deps,
)

utils_a = static_library(
    'utils_a',
    [
        'FileHandle.cpp',
        'SensorPaths.cpp',
        'Utils.cpp',
    ],
    dependencies: default_deps,
)

utils_dep = declare_dependency(
    link_with: [utils_a],
    dependencies: [sdbusplus],
)

devicemgmt_a = static_library(
    'devicemgmt_a',
    [
        'DeviceMgmt.cpp',
    ],
    dependencies: default_deps,
)

devicemgmt_dep = declare_dependency(
    link_with: [devicemgmt_a],
    dependencies: default_deps,
)

pwmsensor_a = static_library(
    'pwmsensor_a',
    'PwmSensor.cpp',
    dependencies: [default_deps, thresholds_dep],
)

pwmsensor_dep = declare_dependency(
    link_with: [pwmsensor_a],
    dependencies: [default_deps, thresholds_dep],
)

peci_incdirs = []
if not meson.get_compiler('cpp').has_header('linux/peci-ioctl.h')
    peci_incdirs = ['../include']
endif

if get_option('intel-cpu').allowed()
    peci_dep = dependency('libpeci', required: true)
endif

if get_option('adc').allowed()
    executable(
        'adcsensor',
        'ADCSensor.cpp',
        'ADCSensorMain.cpp',
        dependencies: [
            default_deps,
            gpiodcxx,
            thresholds_dep,
            utils_dep,
        ],
        install: true,
    )
endif

if get_option('intel-cpu').allowed()
    executable(
        'intelcpusensor',
        'IntelCPUSensorMain.cpp',
        'IntelCPUSensor.cpp',
        dependencies: [
            default_deps,
            gpiodcxx,
            thresholds_dep,
            utils_dep,
            peci_dep,
        ],
        include_directories: peci_incdirs,
        install: true,
    )
endif

if get_option('boot-progress').allowed()
    executable(
        'bootprogress',
        'BootProgressSensor.cpp',
        dependencies: [
            default_deps,
            utils_dep,
        ],
        implicit_include_directories: false,
        include_directories: [
            '../include'
        ] + peci_incdirs,
        install: true,
    )
endif

if get_option('exit-air').allowed()
    executable(
        'exitairtempsensor',
        'ExitAirTempSensor.cpp',
        dependencies: [
            default_deps,
            thresholds_dep,
            utils_dep,
        ],
        install: true,
    )
endif

if get_option('fan').allowed()
    executable(
        'fansensor',
        'FanMain.cpp',
        'TachSensor.cpp',
        'PwmSensor.cpp',
        dependencies: [
            default_deps,
            gpiodcxx,
            i2c,
            thresholds_dep,
            utils_dep,
        ],
        install: true,
    )
endif

if get_option('hwmon-temp').allowed()
    executable(
        'hwmontempsensor',
        'HwmonTempMain.cpp',
        'HwmonTempSensor.cpp',
        dependencies: [
            default_deps,
            devicemgmt_dep,
            thresholds_dep,
            utils_dep,
        ],
        include_directories: '../include',        
        install: true,
    )
endif

if get_option('plx-temp').allowed()
    executable(
        'plxtempsensor',
        'PLXTempMain.cpp',
        'PLXTempSensor.cpp',
        dependencies: [
            default_deps,
            thresholds_dep,
            utils_dep,
        ],
        implicit_include_directories: false,
        include_directories: '../include',
        install: true,
    )
endif

if get_option('intrusion').allowed()
    executable(
        'intrusionsensor',
        'ChassisIntrusionSensor.cpp',
        'IntrusionSensorMain.cpp',
        dependencies: [
            default_deps,
            gpiodcxx,
            i2c,
            utils_dep,
        ],
        install: true,
    )
endif

if get_option('ipmb').allowed()
    executable(
        'ipmbsensor',
        'IpmbSensor.cpp',
        'IpmbSDRSensor.cpp',
        dependencies: [
            default_deps,
            thresholds_dep,
            utils_dep,
        ],
        install: true,
    )
endif

if get_option('mcu').allowed()
    executable(
        'mcutempsensor',
        'MCUTempSensor.cpp',
        dependencies: [
            default_deps,
            i2c,
            thresholds_dep,
            utils_dep,
        ],
        install: true,
    )
endif

if get_option('nvme').allowed()
    nvme_srcs = files('NVMeSensor.cpp', 'NVMeSensorMain.cpp')
    nvme_srcs += files('NVMeBasicContext.cpp')

    nvme_deps = [default_deps, i2c, thresholds_dep, utils_dep, threads]

    executable(
        'nvmesensor',
        sources: nvme_srcs,
        dependencies: nvme_deps,
        install: true,
    )
endif

if get_option('psu').allowed()
    executable(
        'psusensor',
        'PSUEvent.cpp',
        'PSUSensor.cpp',
        'PSUSensorMain.cpp',
        dependencies: [
            default_deps,
            devicemgmt_dep,
            pwmsensor_dep,
            thresholds_dep,
            utils_dep,
        ],
        install: true,
    )
endif

if get_option('external').allowed()
    executable(
        'externalsensor',
        'ExternalSensor.cpp',
        'ExternalSensorMain.cpp',
        dependencies: [
            default_deps,
            thresholds_dep,
            utils_dep,
        ],
        install: true,
    )
endif

if get_option('procstatus').allowed()
    executable(
        'processorstatus',
        'ProcessorStatus.cpp',
        'ProcessorStatusMain.cpp',
        dependencies: [
            default_deps,
            gpiodcxx,
            utils_dep,
        ],
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
        dependencies: [
            default_deps,
            i2c,
            utils_dep,
        ],
        implicit_include_directories: false,
        include_directories: '../include',
        install: true,
    )
endif

if get_option('ipmbstatus').allowed()
    executable(
        'ipmbstatus',
        'IpmbStatus.cpp',
        dependencies: [
            default_deps,
            thresholds_dep,
            utils_dep,
        ],
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
        dependencies: [
            default_deps,
            utils_dep,
        ],
        implicit_include_directories: false,
        include_directories: '../include',
        install: true,
    )
endif

if get_option('gpu').allowed()
    executable(
        'gpustatus',
        'GPUStatus.cpp',
        'GPUStatusMain.cpp',
        dependencies: [
            default_deps,
            utils_dep,
        ],
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
        dependencies: [
            default_deps,
            utils_dep,
        ],
        implicit_include_directories: false,
        include_directories: '../include',
        install: true,
    )
endif

if get_option('satellite').enabled()
    executable(
        'satellitesensor',
        'SatelliteSensor.cpp',
        dependencies: [
            default_deps,
            i2c,
            thresholds_dep,
            utils_dep,
        ],
        implicit_include_directories: false,
        include_directories: include_directories(['../include','../src']),
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
            devicemgmt_dep
        ],
        install: true,
    )
endif
