// bindless_suite.hpp - tests built on a bindless layout (src/bindless_layout.* + shaders/bindless/).
// One pipeline layout for every test; resources are bindless slots, each test only sets push constants.
// Every test has a CPU reference: --verify checks the result of each thread.
#pragma once

#include "gpu.hpp"

struct BindlessSuite;

std::vector<std::string>    bindless_test_names();
BindlessSuite*              bindless_create( Gpu& gpu, const AppOptions& opt, const std::string& shader_dir );
void                        bindless_append_tests( BindlessSuite* s, std::vector<AppRunnableTest>& out );
bool                        bindless_verify( Gpu& gpu, BindlessSuite* s );
void                        bindless_destroy( Gpu& gpu, BindlessSuite* s );
