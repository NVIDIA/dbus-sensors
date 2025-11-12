nvme_srcs = files('NVMeMiSensor.cpp', 'NVMeMiSensorMain.cpp')
nvme_srcs += files('NVMeMiContext.cpp')
nvme_srcs += files('NVMeMiStatusSensor.cpp')
nvme_srcs += files('NVMeMiManager.cpp')

libnvme = dependency('libnvme')
libnvme_mi = dependency('libnvme-mi')

nvme_deps = [
    default_deps,
    i2c,
    thresholds_dep,
    utils_dep,
    threads,
    libnvme,
    libnvme_mi,
]
src_inc = include_directories('..')

executable(
    'misensor',
    sources: nvme_srcs,
    dependencies: nvme_deps,
    include_directories: src_inc,
    install: true,
)

