gtest = dependency('gtest', main : true)
if not gtest.found()
    gtest_proj = import('cmake').subproject('googletest', required: false)
    if gtest_proj.found()
        gtest = declare_dependency(
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

# Test executables
test_instance_id = executable(
    'test_instance_id',
    'test_instance_id.cpp',
    dependencies : [gtest],
    link_with : [gpuserver_lib],
    include_directories : [
        include_directories('../lib'),
        include_directories('../server')
    ]
)

test_base = executable(
    'test_base',
    'test_base.cpp',
    dependencies : [gtest],
    link_with : [gpuserver_lib],
    include_directories : [
        include_directories('../lib'),
        include_directories('../server')
    ]
)

test_utils = executable(
    'test_utils',
    'test_utils.cpp',
    dependencies : [
        gtest,
        gpuserver_lib_dep
    ],
    include_directories : [
        include_directories('../lib'),
        include_directories('../server')
    ]
)

# Register tests
test('instance_id_tests', test_instance_id)
test('base_tests', test_base)
test('utils_tests', test_utils) 
