gtest_dep = dependency('gtest', main: true, disabler: true, required: false)
if not gtest_dep.found()
    gtest_proj = import('cmake').subproject('googletest', required: false)
    if gtest_proj.found()
        gtest_dep = declare_dependency(
            dependencies: [
                dependency('threads'),
                gtest_proj.dependency('gtest'),
                gtest_proj.dependency('gtest_main'),
            ],
        )
    else
        assert(
            not get_option('tests').allowed(),
            'Googletest is required if tests are enabled',
        )
    endif
endif

test(
    'test_vhmc_shm_reader',
    executable(
        'test_vhmc_shm_reader',
        'test_vHMCShmReader.cpp',
        '../vHMCShmReader.cpp',
        dependencies: [gtest_dep, default_deps],
        implicit_include_directories: false,
        include_directories: include_directories('..'),
    ),
)
