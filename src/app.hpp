// MIT License. Original PerfTest (c) 2016-2017 Sebastian Aaltonen. Vulkan port (c) 2026 Gabriel Sassone.
#pragma once

#include <string>
#include <vector>
#include <functional>
#include <cstdint>

// Forward declarations
struct Gpu;
typedef struct VkCommandBuffer_T* VkCommandBuffer;

// AppOptions ///////////////////////////////////////////////////////////////
struct AppOptions {

    int             device_index    = -1;       // -1 = first discrete GPU, else first device
    std::string     shader_dir;
    int             warmup_frames   = 30;
    int             bench_frames    = 30;
    uint32_t        groups_x        = 4;        // Original: 1024x1024 threads, 256x1 groups -> 4x1024 groups
    uint32_t        groups_y        = 1024;
    bool            validation      = false;
    bool            list_only       = false;
    bool            verify          = false;
    bool            shuffle         = false;    // new dispatch order every frame
    bool            robustness      = false;    // robustBufferAccess (+ robustBufferAccess2 / robustImageAccess2 if available)
    uint32_t        subgroup_size   = 0;        // 0 = driver default; else required subgroup size for every pipeline
    std::string     suites          = "original,bindless";
    std::string     filter;
    std::string     csv_path;
};

AppOptions          app_parse_options( int argc, char** argv );
bool                app_suite_enabled( const AppOptions& opt, const char* suite );
std::string         app_find_shader_dir( const AppOptions& opt, const char* argv0 );

// Errors ////////////////////////////////////////////////////////////////
[[noreturn]] void app_fatal( const char* fmt, ... );

#define VK_CHECK( call )                                                                    \
    do {                                                                                    \
        VkResult result_ = ( call );                                                        \
        if ( result_ != VK_SUCCESS ) app_fatal( "%s failed (VkResult %d)", #call, result_ );    \
    } while ( 0 )

// Files /////////////////////////////////////////////////////////////////
bool                app_file_exists( const std::string& path );
std::vector<uint32_t> app_read_spirv( const std::string& path );
std::string         app_read_text_first_line( const std::string& path );
std::string         app_directory_of( const std::string& path );

// Runnable tests ////////////////////////////////////////////////////////
struct AppRunnableTest {
    std::string                           suite;         // "original", "bindless"
    std::string                           name;
    std::string                           skip_reason;   // non-empty = not runnable
    std::function<void( VkCommandBuffer )> record;       // bind + push + dispatch (no barriers, no timestamps)
    std::vector<double>                   samples_ms;    // one per benchmark frame
}; // struct AppRunnableTest

struct AppStats {
    double  total_ms = 0.0;   // sum over the benchmark frames (the original's metric)
    double  median_ms = 0.0;   // per dispatch
    double  min_ms = 0.0;
    double  cv_pct = 0.0;   // standard deviation / mean
}; // struct AppStats

AppStats    app_compute_stats( const std::vector<double>& samples );

// Prints "name: X.XXXms Y.YYYx" (original format, ratio vs Buffer<RGBA8>.Load random) and writes the CSV.
void        app_report_results( const Gpu& gpu, const AppOptions& opt, const std::string& compiler, const std::vector<AppRunnableTest>& tests );
