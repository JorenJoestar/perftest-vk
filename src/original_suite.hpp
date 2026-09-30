// original_suite.hpp - the 138 original PerfTest tests: unchanged shaders, names and output format.
// Each test has its own small descriptor set: binding 0 cbuffer, 1 source, 2 RWBuffer<float> output, 3 sampler.
#pragma once

#include "gpu.hpp"

struct OriginalSuite;

std::vector<std::string> original_test_names();
OriginalSuite*           original_create( Gpu& gpu, const AppOptions& opt, const std::string& shader_dir );
void                     original_append_tests( OriginalSuite* s, std::vector<AppRunnableTest>& out );
void                     original_destroy( Gpu& gpu, OriginalSuite* s );
