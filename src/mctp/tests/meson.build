gtest_dep = dependency('gtest', main: true, disabler: true, required: false)
gmock_dep = dependency('gmock', disabler: true, required: false)
if not gtest_dep.found() or not gmock_dep.found()
    gtest_proj = import('cmake').subproject('googletest', required: false)
    if gtest_proj.found()
        gtest_dep = declare_dependency(
            dependencies: [
                dependency('threads'),
                gtest_proj.dependency('gtest'),
                gtest_proj.dependency('gtest_main'),
            ],
        )
        gmock_dep = gtest_proj.dependency('gmock')
    else
        assert(
            not get_option('tests').allowed(),
            'Googletest is required if tests are enabled',
        )
    endif
endif

ut_deps_list = [gtest_dep]
libusb_dep = dependency('libusb-1.0')

ut_deps_list += default_deps

subobject_linkage_disable_args = []
if meson.get_compiler('cpp').has_argument('-Wno-subobject-linkage')
    subobject_linkage_disable_args += ['-Wno-subobject-linkage']
endif

# The project enables Boost.Asio io_uring globally for daemon builds, but unit
# tests should not depend on the kernel/container io_uring resource limits.
boost_asio_test_backend_args = [
    '-UBOOST_ASIO_HAS_IO_URING',
    '-DBOOST_ASIO_DISABLE_IO_URING',
    '-UBOOST_ASIO_DISABLE_EPOLL',
]

# ld --wrap flags to intercept sd-bus calls in fake-connection tests
sd_bus_wrap_link_args = [
    '-Wl,--wrap,sd_bus_get_fd',
    '-Wl,--wrap,sd_bus_get_events',
    '-Wl,--wrap,sd_bus_get_timeout',
    '-Wl,--wrap,sd_bus_add_match',
    '-Wl,--wrap,sd_bus_call',
    '-Wl,--wrap,sd_bus_process',
    '-Wl,--wrap,sd_bus_slot_unref',
    '-Wl,--wrap,sd_bus_add_object_manager',
]

# Extended wrap flags that also intercept socket/system/sendto calls
socket_wrap_link_args = sd_bus_wrap_link_args + [
    '-Wl,--wrap,socket',
    '-Wl,--wrap,setsockopt',
    '-Wl,--wrap,bind',
    '-Wl,--wrap,system',
    '-Wl,--wrap,symlink',
    '-Wl,--wrap,if_nametoindex',
    '-Wl,--wrap,sendto',
    '-Wl,--wrap,recvfrom',
    '-Wl,--wrap,sd_bus_default',
    '-Wl,--wrap,sd_bus_request_name',
    '-Wl,--wrap,sd_bus_message_new_method_call',
    '-Wl,--wrap,sd_bus_call_async',
]

heartbeat_wrap_link_args = socket_wrap_link_args + [
    '-Wl,--wrap,sd_event_new',
    '-Wl,--wrap,sd_event_add_time_relative',
]

# Additional wrap for tests that link MCTPCustomDevices.cpp (defines writeSysfsFile)
custom_devices_wrap_link_args = socket_wrap_link_args + [
    '-Wl,--wrap,_Z14writeSysfsFileRKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEES6_',
]

reactor_main_wrap_link_args = custom_devices_wrap_link_args + [
    '-Wl,--wrap,main',
    '-Wl,--wrap,uname',
    '-Wl,--wrap,_ZNSt10filesystem6statusERKNS_7__cxx114pathE',
]

test(
    'MCTPReactor',
    executable(
        'test_MCTPReactor',
        'test_MCTPReactor.cpp',
        '../MCTPReactor.cpp',
        '../USBRecovery.cpp',
        '../MCTPEndpoint.cpp',
        '../MCTPEndpointUtils.cpp',
        'phosphor_logging_drain.cpp',
        cpp_args: boost_asio_test_backend_args + ['-fno-access-control'],
        dependencies: [gmock_dep, ut_deps_list, utils_dep, libusb_dep],
        implicit_include_directories: false,
        include_directories: '..',
    ),
)

# Intercept libusb descriptor fetches so findBulkOutEndpoint can be driven
# through all branches with crafted descriptors (no real USB device needed).
usb_recovery_wrap_link_args = [
    '-Wl,--wrap,libusb_get_device_descriptor',
    '-Wl,--wrap,libusb_get_active_config_descriptor',
    '-Wl,--wrap,libusb_get_config_descriptor',
    '-Wl,--wrap,libusb_free_config_descriptor',
    '-Wl,--wrap,libusb_wrap_sys_device',
    '-Wl,--wrap,libusb_get_device',
    '-Wl,--wrap,libusb_kernel_driver_active',
    '-Wl,--wrap,libusb_detach_kernel_driver',
    '-Wl,--wrap,libusb_attach_kernel_driver',
    '-Wl,--wrap,libusb_claim_interface',
    '-Wl,--wrap,libusb_release_interface',
    '-Wl,--wrap,libusb_clear_halt',
    '-Wl,--wrap,libusb_init',
    '-Wl,--wrap,libusb_close',
    '-Wl,--wrap,libusb_exit',
    '-Wl,--wrap,open',
    '-Wl,--wrap,open64',
    '-Wl,--wrap,_ZNSt10filesystem16weakly_canonicalERKNS_7__cxx114pathERSt10error_code',
]

test(
    'USBRecovery',
    executable(
        'test_USBRecovery',
        'test_USBRecovery.cpp',
        'phosphor_logging_drain.cpp',
        cpp_args: boost_asio_test_backend_args,
        link_args: usb_recovery_wrap_link_args,
        dependencies: [gmock_dep, ut_deps_list, utils_dep, libusb_dep],
        implicit_include_directories: false,
        include_directories: '..',
    ),
)

test(
    'MCTPEndpoint',
    executable(
        'test_MCTPEndpoint',
        'test_MCTPEndpoint.cpp',
        'test_MCTPDeviceRepository.cpp',
        '../MCTPEndpoint.cpp',
        '../MCTPEndpointUtils.cpp',
        'sd_bus_wrappers.cpp',
        'filesystem_wrappers.cpp',
        'phosphor_logging_drain.cpp',
        cpp_args: boost_asio_test_backend_args + ['-fno-access-control'],
        link_args: socket_wrap_link_args,
        dependencies: [gmock_dep, ut_deps_list, utils_dep],
        implicit_include_directories: false,
        include_directories: '..',
    ),
)

test(
    'MCTPDeviceRepository',
    executable(
        'test_MCTPDeviceRepository',
        'test_MCTPDeviceRepository.cpp',
        '../MCTPEndpoint.cpp',
        '../MCTPEndpointUtils.cpp',
        'phosphor_logging_drain.cpp',
        cpp_args: boost_asio_test_backend_args,
        dependencies: [gmock_dep, ut_deps_list, utils_dep],
        implicit_include_directories: false,
        include_directories: '..',
    ),
)

test(
    'MCTPEndpointUtils',
    executable(
        'test_MCTPEndpointUtils',
        'test_MCTPEndpointUtils.cpp',
        'sd_bus_wrappers.cpp',
        'phosphor_logging_drain.cpp',
        cpp_args: boost_asio_test_backend_args,
        link_args: socket_wrap_link_args,
        dependencies: [ut_deps_list, utils_dep],
        implicit_include_directories: false,
        include_directories: '..',
    ),
)

test(
    'MCTPCustomDevices',
    executable(
        'test_MCTPCustomDevices',
        'test_MCTPCustomDevices.cpp',
        '../MCTPCustomDevices.cpp',
        '../MCTPEndpoint.cpp',
        '../MCTPEndpointUtils.cpp',
        'sd_bus_wrappers.cpp',
        'filesystem_wrappers.cpp',
        'phosphor_logging_drain.cpp',
        cpp_args: boost_asio_test_backend_args + ['-fno-access-control'],
        link_args: custom_devices_wrap_link_args + [
            '-Wl,--wrap,_ZNSt14basic_ifstreamIcSt11char_traitsIcEEC1EPKcSt13_Ios_Openmode',
        ],
        dependencies: [gmock_dep, ut_deps_list, utils_dep],
        implicit_include_directories: false,
        include_directories: '..',
    ),
)

test(
    'MCTPBridgePoolDevice',
    executable(
        'test_MCTPBridgePoolDevice',
        'test_MCTPBridgePoolDevice.cpp',
        '../MCTPBridgePoolDevice.cpp',
        '../MCTPEndpoint.cpp',
        '../MCTPEndpointUtils.cpp',
        'sd_bus_wrappers.cpp',
        'filesystem_wrappers.cpp',
        'phosphor_logging_drain.cpp',
        cpp_args: boost_asio_test_backend_args + ['-fno-access-control'],
        link_args: socket_wrap_link_args,
        dependencies: [gmock_dep, ut_deps_list, utils_dep],
        implicit_include_directories: false,
        include_directories: '..',
    ),
)

test(
    'MCTPReactorMain',
    executable(
        'test_MCTPReactorMain',
        'test_MCTPReactorMain.cpp',
        '../MCTPReactor.cpp',
        '../USBRecovery.cpp',
        '../MCTPEndpoint.cpp',
        '../MCTPEndpointUtils.cpp',
        '../MCTPCustomDevices.cpp',
        '../MCTPBridgePoolDevice.cpp',
        'sd_bus_wrappers.cpp',
        'filesystem_wrappers.cpp',
        'reactor_main_interposers.cpp',
        'phosphor_logging_drain.cpp',
        cpp_args: subobject_linkage_disable_args + boost_asio_test_backend_args + [
            '-fno-access-control',
        ],
        link_args: reactor_main_wrap_link_args,
        dependencies: [gmock_dep, default_deps, utils_dep, libusb_dep],
        implicit_include_directories: false,
        include_directories: '..',
    ),
)

test(
    'MCTPHeartBeatApp',
    executable(
        'test_MCTPHeartBeatApp',
        'test_MCTPHeartBeatApp.cpp',
        'sd_bus_wrappers.cpp',
        cpp_args: subobject_linkage_disable_args + boost_asio_test_backend_args + [
            '-fno-access-control',
            '-DMCTP_HEARTBEAT_RESTART_NOTIFICATION=' + (
                get_option('mctp-heartbeat-restart-notification').enabled() ? 'true' : 'false'
            ),
        ],
        link_args: heartbeat_wrap_link_args,
        dependencies: [gmock_dep, ut_deps_list, utils_dep],
        implicit_include_directories: false,
        include_directories: '..',
    ),
    timeout: 180,
)
