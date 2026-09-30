
#include "app.hpp"
#include "gpu.hpp"

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <random>


void app_fatal( const char* fmt, ... ) {
    va_list args;
    va_start( args, fmt );

    fflush( stdout );
    fprintf( stderr, "error: " );
    vfprintf( stderr, fmt, args );
    fprintf( stderr, "\n" );

    va_end( args );

    exit( 1 );
}

bool app_file_exists( const std::string& path ) {
    std::ifstream f( path, std::ios::binary );

    return f.good();
}

std::vector<uint32_t> app_read_spirv( const std::string& path ) {

    std::ifstream f( path, std::ios::binary | std::ios::ate );
    if ( !f ) {
        return {};
    }

    const size_t size = ( size_t )f.tellg();
    if ( size == 0 || size % 4 ) {
        return {};
    }

    std::vector<uint32_t> code( size / 4 );
    f.seekg( 0 );
    f.read( ( char* )code.data(), ( std::streamsize )size );

    return code;
}

std::string app_read_text_first_line( const std::string& path ) {
    std::ifstream f( path );
    std::string line;

    if ( f ) {
        std::getline( f, line );
    }
    return line;
}

std::string app_directory_of( const std::string& path ) {
    const size_t slash = path.find_last_of( "/\\" );
    return slash == std::string::npos ? std::string( "." ) : path.substr( 0, slash );
}

std::string app_find_shader_dir( const AppOptions& opt, const char* argv0 ) {

    const std::string exe_dir = app_directory_of( argv0 );

    if ( !opt.shader_dir.empty() ) {
        if ( app_file_exists( opt.shader_dir + "/loadRaw1dLinear.spv" ) || app_file_exists( opt.shader_dir + "/bindless_fill.spv" ) ) {
            return opt.shader_dir;
        }

        std::string found;
        for ( const char* base : { "/shaders/", "/../shaders/" } ) {
            for ( const char* compiler : { "slang", "dxc", "glslang" } ) {
                const std::string dir = exe_dir + base + compiler;
                if ( app_file_exists( dir + "/loadRaw1dLinear.spv" ) ) {
                    found += "\n  " + dir;
                }
            }
        }
        fprintf( stderr, "error: no compiled shaders in '%s' (the path is relative to the current directory).\n", opt.shader_dir.c_str() );
        fprintf( stderr, "%s%s\n", found.empty() ? "No shader sets found next to the executable either."
                 : "Shader sets found next to the executable:", found.c_str() );
        exit( 1 );
    }

    for ( const char* base : { "/shaders/", "/../shaders/" } ) {
        for ( const char* compiler : { "slang", "dxc", "glslang" } ) {
            const std::string dir = exe_dir + base + compiler;
            if ( app_file_exists( dir + "/loadRaw1dLinear.spv" ) ) {
                return dir;
            }
        }
    }

    for ( const char* compiler : { "slang", "dxc", "glslang" } ) {
        const std::string dir = std::string( "build/shaders/" ) + compiler;
        if ( app_file_exists( dir + "/loadRaw1dLinear.spv" ) ) {
            return dir;
        }
    }

    app_fatal( "compiled shaders not found; build them (scripts/compile_shaders.py) or pass --shaders DIR" );
}

static void print_usage() {
    printf( "Usage: perftest-vk [options]\n"
            "  --device N          physical device index (default: first discrete GPU)\n"
            "  --shaders DIR       directory with compiled .spv files (default: shaders/<slang|dxc|glslang> next to the exe)\n"
            "  --suites LIST       comma-separated: original,bindless (default: both)\n"
            "  --filter TEXT       run only tests whose name contains TEXT\n"
            "  --warmup N          warm-up frames (default 30)\n"
            "  --frames N          benchmark frames (default 30); statistics are computed over these\n"
            "  --shuffle           new dispatch order every frame (reduces order and clock-ramp bias)\n"
            "  --groups X Y        dispatch size of the original tests (default 4 1024, like the original)\n"
            "  --robustness        enable robustBufferAccess (+ robustBufferAccess2/robustImageAccess2 if available)\n"
            "  --subgroup-size N   require subgroup size N for every pipeline (e.g. 32 or 64 on RDNA; Vulkan 1.3)\n"
            "  --verify            check results instead of timing (bindless suite)\n"
            "  --csv FILE          also write results as CSV (with device, driver, compiler, config columns)\n"
            "  --validation        enable VK_LAYER_KHRONOS_validation\n"
            "  --list              list devices and tests, then exit\n" );
}

AppOptions app_parse_options( int argc, char** argv ) {
    AppOptions o;
    for ( int i = 1; i < argc; ++i ) {
        std::string a = argv[ i ];

        auto next = [ & ]() -> const char* {
            if ( i + 1 >= argc ) app_fatal( "missing value for %s", a.c_str() );
            return argv[ ++i ];
            };

        if ( a == "--device" ) {
            o.device_index = atoi( next() );
        } else if ( a == "--shaders" ) {
            o.shader_dir = next();
        } else if ( a == "--suites" ) {
            o.suites = next();
        } else if ( a == "--warmup" ) {
            o.warmup_frames = atoi( next() );
        } else if ( a == "--frames" ) {
            o.bench_frames = atoi( next() );
        } else if ( a == "--groups" ) {
            o.groups_x = ( uint32_t )atoi( next() );
            o.groups_y = ( uint32_t )atoi( next() );
        } else if ( a == "--filter" ) {
            o.filter = next();
        } else if ( a == "--shuffle" ) {
            o.shuffle = true;
        } else if ( a == "--robustness" ) {
            o.robustness = true;
        } else if ( a == "--subgroup-size" ) {
            o.subgroup_size = ( uint32_t )atoi( next() );
        } else if ( a == "--csv" ) {
            o.csv_path = next();
        } else if ( a == "--validation" ) {
            o.validation = true;
        } else if ( a == "--list" ) {
            o.list_only = true;
        } else if ( a == "--verify" ) {
            o.verify = true;
        } else if ( a == "--help" || a == "-h" ) {
            print_usage();
            exit( 0 );
        } else if ( i == 1 && isdigit( ( unsigned char )a[ 0 ] ) ) {
            o.device_index = atoi( a.c_str() );   // original: PerfTest.exe [ADAPTER_INDEX]
        } else {
            print_usage();
            app_fatal( "unknown option %s", a.c_str() );
        }
    }
    if ( o.bench_frames < 1 ) {
        o.bench_frames = 1;
    }

    if ( o.warmup_frames < 0 ) {
        o.warmup_frames = 0;
    }

    return o;
}

bool app_suite_enabled( const AppOptions& opt, const char* suite ) {

    // reject unknown names instead of silently running nothing
    for ( size_t begin = 0; begin <= opt.suites.size(); ) {
        const size_t end = std::min( opt.suites.find( ',', begin ), opt.suites.size() );
        const std::string name = opt.suites.substr( begin, end - begin );

        if ( name != "original" && name != "bindless" ) {
            app_fatal( "unknown suite '%s' in --suites (valid: original, bindless)", name.c_str() );
        }

        begin = end + 1;
    }

    const std::string list = "," + opt.suites + ",";
    return list.find( std::string( "," ) + suite + "," ) != std::string::npos;
}


AppStats app_compute_stats( const std::vector<double>& samples ) {
    AppStats s;

    if ( samples.empty() ) {
        return s;
    }

    std::vector<double> sorted = samples;
    std::sort( sorted.begin(), sorted.end() );
    
    const size_t n = sorted.size();
    
    s.total_ms = std::accumulate( sorted.begin(), sorted.end(), 0.0 );
    s.median_ms = n % 2 ? sorted[ n / 2 ] : 0.5 * ( sorted[ n / 2 - 1 ] + sorted[ n / 2 ] );
    s.min_ms = sorted.front();
    
    const double mean = s.total_ms / ( double )n;
    
    double var = 0.0;
    
    for ( double v : sorted ) {
        var += ( v - mean ) * ( v - mean );
    }

    var /= ( double )( n > 1 ? n - 1 : 1 );
    
    s.cv_pct = mean > 0.0 ? 100.0 * std::sqrt( var ) / mean : 0.0;
    
    return s;
}

static std::string csv_quote( const std::string& s ) {

    std::string out = "\"";
    for ( char c : s ) {
        if ( c == '"' ) {
            out += '"';
        }
        out += c;
    }
    return out + "\"";
}

void app_report_results( const Gpu& gpu, const AppOptions& opt, const std::string& compiler, const std::vector<AppRunnableTest>& tests ) {
    // Reference: the original's "Buffer<RGBA8>.Load random", else the first runnable test.
    std::string compare_case = "Buffer<RGBA8>.Load random";
    double compare_time = 0.0;
    for ( const AppRunnableTest& t : tests ) {
        if ( t.name == compare_case && !t.samples_ms.empty() ) {
            compare_time = app_compute_stats( t.samples_ms ).total_ms;
        }
    }

    if ( compare_time <= 0.0 ) {
        for ( const AppRunnableTest& t : tests ) {
            if ( !t.samples_ms.empty() ) {
                compare_case = t.name;
                compare_time = app_compute_stats( t.samples_ms ).total_ms;
                break;
            }
        }
    }

    printf( "\nPerformance compared to %s\n\n", compare_case.c_str() );

    FILE* csv = opt.csv_path.empty() ? nullptr : fopen( opt.csv_path.c_str(), "w" );

    if ( !opt.csv_path.empty() && !csv ) {
        fprintf( stderr, "warning: cannot write %s\n", opt.csv_path.c_str() );
    }

    char device_id[ 16 ], vendor_id[ 16 ], api[ 32 ];
    snprintf( vendor_id, sizeof( vendor_id ), "0x%04X", gpu.props.vendorID );
    snprintf( device_id, sizeof( device_id ), "0x%04X", gpu.props.deviceID );
    snprintf( api, sizeof( api ), "%u.%u.%u", VK_API_VERSION_MAJOR( gpu.props.apiVersion ), VK_API_VERSION_MINOR( gpu.props.apiVersion ),
              VK_API_VERSION_PATCH( gpu.props.apiVersion ) );

    const std::string meta = "," + csv_quote( gpu.props.deviceName ) + "," + vendor_id + "," + device_id + "," +
                             csv_quote( gpu_driver_version_string( gpu.props ) ) + "," + api + "," + csv_quote( compiler ) + "," +
                             csv_quote( gpu_config_string( gpu, opt ) );

    if ( csv ) {
        fprintf( csv, "suite,test,total_ms,ratio,median_ms,min_ms,cv_pct,status,device,vendor_id,device_id,driver,vulkan,compiler,config\n" );
    }

    std::string current_suite;
    int noisy = 0;
    for ( const AppRunnableTest& t : tests ) {
        if ( t.suite != current_suite ) {
            current_suite = t.suite;
            if ( t.suite == "bindless" ) {
                printf( "\nBindless suite:\n" );
            }
        }
        if ( !t.skip_reason.empty() ) {
            printf( "%s: skipped (%s)\n", t.name.c_str(), t.skip_reason.c_str() );
            if ( csv ) {
                fprintf( csv, "%s,%s,,,,,,%s%s\n", t.suite.c_str(), csv_quote( t.name ).c_str(), csv_quote( "skipped: " + t.skip_reason ).c_str(), meta.c_str() );
            }
            continue;
        }
        const AppStats s = app_compute_stats( t.samples_ms );
        const double ratio = s.total_ms > 0.0 ? compare_time / s.total_ms : 0.0;
        
        printf( "%s: %.3fms %.3fx", t.name.c_str(), s.total_ms, ratio );
        
        if ( s.cv_pct > 5.0 ) {
            printf( "  (noisy: cv %.0f%%)", s.cv_pct );
            ++noisy;
        }
        
        printf( "\n" );
        
        if ( csv ) {
            fprintf( csv, "%s,%s,%.4f,%.4f,%.5f,%.5f,%.2f,ok%s\n", t.suite.c_str(), csv_quote( t.name ).c_str(), s.total_ms, ratio,
                     s.median_ms, s.min_ms, s.cv_pct, meta.c_str() );
        }
    }
    
    if ( noisy ) {
        printf( "\n%d tests varied more than 5%% between frames: on laptops use AC power and maximum performance, and more --frames.\n", noisy );
    }

    if ( csv ) {
        fclose( csv );
    }
}
