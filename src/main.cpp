// perftest-vk: Vulkan port of Sebastian Aaltonen's PerfTest (https://github.com/sebbbi/perftest), extended.
//
// Suites:
//   original  the 138 PerfTest tests: shaders compiled unchanged, same names, same output format
//   bindless  tests on a bindless layout (one pipeline layout, push constants only), each with a CPU reference
//
// MIT License. Original PerfTest (c) 2016-2017 Sebastian Aaltonen. Vulkan port (c) 2026 Gabriel Sassone.
#include "bindless_suite.hpp"
#include "gpu.hpp"
#include "original_suite.hpp"
#include "app.hpp"

#include <cstdio>

int main( int argc, char** argv ) {
    const AppOptions opt = app_parse_options( argc, argv );
    const bool run_original = app_suite_enabled( opt, "original" );
    const bool run_bindless = app_suite_enabled( opt, "bindless" );

    printf( "PerfTest (Vulkan)\nTo select device, use: perftest-vk --device N (or perftest-vk N)\n\n" );

    Gpu gpu;
    gpu_create( gpu, opt );

    if ( opt.list_only ) {
        if ( run_original ) {
            for ( const std::string& n : original_test_names() ) {
                printf( "%s\n", n.c_str() );
            }
        }

        if ( run_bindless ) {
            for ( const std::string& n : bindless_test_names() ) {
                printf( "%s\n", n.c_str() );
            }
        }

        gpu_destroy( gpu );

        return 0;
    }

    const std::string shader_dir = app_find_shader_dir( opt, argv[ 0 ] );
    std::string compiler = app_read_text_first_line( shader_dir + "/compiler.txt" );

    if ( compiler.empty() ) {
        compiler = "unknown compiler";
    }

    printf( "Device:   %s (%s, vendor 0x%04X, device 0x%04X)\n", gpu.props.deviceName, gpu_vendor_name( gpu.props.vendorID ), gpu.props.vendorID, gpu.props.deviceID );
    printf( "Driver:   %s, Vulkan %u.%u.%u\n", gpu_driver_version_string( gpu.props ).c_str(),
            VK_API_VERSION_MAJOR( gpu.props.apiVersion ), VK_API_VERSION_MINOR( gpu.props.apiVersion ), VK_API_VERSION_PATCH( gpu.props.apiVersion ) );

    printf( "Shaders:  %s (%s)\n", shader_dir.c_str(), compiler.c_str() );
    printf( "Dispatch: %u x %u thread groups of 256 threads, %d warm-up + %d benchmark frames\n",
            opt.groups_x, opt.groups_y, opt.warmup_frames, opt.bench_frames );

    printf( "Config:   %s (subgroup size %u%s)\n", gpu_config_string( gpu, opt ).c_str(), gpu.required_subgroup_size ? gpu.required_subgroup_size : gpu.subgroup_size,
            gpu.required_subgroup_size ? ", required" : ", default" );

    if ( !gpu.features.storage_write_without_format || !gpu.features.storage_read_without_format ) {
        printf( "Warning:  shaderStorageImage%sWithoutFormat not supported; Slang declares both capabilities for RWBuffer<float>,\n"
                "          so Slang-compiled pipelines may be rejected (use the dxc or glslang shader set).\n",
                !gpu.features.storage_write_without_format ? "Write" : "Read" );
    }

    if ( run_bindless && !gpu.features.bindless ) {
        printf( "Bindless: suite skipped (%s)\n", gpu.features.bindless_missing.c_str() );
    }

    printf( "\n" );

    OriginalSuite* original = run_original ? original_create( gpu, opt, shader_dir ) : nullptr;
    BindlessSuite* bindless = run_bindless ? bindless_create( gpu, opt, shader_dir ) : nullptr;

    int exit_code = 0;

    if ( opt.verify ) {

        if ( !bindless ) {
            app_fatal( "--verify checks the bindless suite: add it to --suites" );
        }

        bool ok = true;

        if ( bindless ) {
            ok &= bindless_verify( gpu, bindless );
        }

        printf( "\n%s\n", ok ? "All verified tests passed." : "Some tests returned WRONG results (see FAIL lines)." );
        exit_code = ok ? 0 : 3;

    } else {
        std::vector<AppRunnableTest> tests;

        if ( original ) {
            original_append_tests( original, tests );
        }

        if ( bindless ) {
            bindless_append_tests( bindless, tests );
        }

        if ( tests.empty() ) {
            app_fatal( "no tests match the filter" );
        }

        std::vector<AppRunnableTest*> active;

        for ( AppRunnableTest& t : tests ) {
            if ( t.skip_reason.empty() ) {
                active.push_back( &t );
            }
        }

        if ( active.empty() ) {
            app_fatal( "no runnable tests" );
        }

        gpu_run_benchmark( gpu, opt, active );
        app_report_results( gpu, opt, compiler, tests );
    }

    vkDeviceWaitIdle( gpu.device );

    if ( original ) {
        original_destroy( gpu, original );
    }

    if ( bindless ) {
        bindless_destroy( gpu, bindless );
    }

    gpu_destroy( gpu );

    if ( opt.validation ) {
        printf( "\nValidation messages: %u\n", g_validation_messages );

        if ( g_validation_messages && exit_code == 0 ) {
            exit_code = 2;
        }
    }
    return exit_code;
}
