#!/usr/bin/env python3
"""Compile the perftest HLSL shaders to SPIR-V.

The original HLSL files from sebbbi/perftest are compiled UNCHANGED. HLSL registers are
remapped to Vulkan bindings with the compilers' register-shift options:

    b0 (cbuffer)          -> binding 0
    t0 (SRV)              -> binding 1
    u0 (UAV)              -> binding 2
    s0 (sampler)          -> binding 3

The bindless shaders (shaders/bindless) use explicit [[vk::binding]] annotations instead.

Usage:
    python scripts/compile_shaders.py --compiler slang   [--exe PATH] [--out build/shaders]
    python scripts/compile_shaders.py --compiler dxc     [--exe PATH]
    python scripts/compile_shaders.py --compiler glslang [--exe PATH]
"""

import argparse
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SHADER_DIR = os.path.join(ROOT, "shaders")
BINDLESS_DIR = os.path.join(SHADER_DIR, "bindless")

DEFAULT_EXE = {"slang": "slangc", "dxc": "dxc", "glslang": "glslangValidator"}


def command_for(compiler, exe, src, dst, kind):
    shifts = ["-fvk-b-shift", "0", "0", "-fvk-t-shift", "1", "0", "-fvk-u-shift", "2", "0", "-fvk-s-shift", "3", "0"]
    if compiler == "slang":
        profile = "spirv_1_5" if kind == "bindless" else "spirv_1_3"
        cmd = [exe, src, "-target", "spirv", "-entry", "main", "-stage", "compute", "-profile", profile, "-O2", "-o", dst]
        if kind == "original":
            cmd += shifts
        if kind == "bindless":
            cmd += ["-warnings-disable", "39001,41012"]   # intentional binding aliasing,
        return cmd
    if compiler == "dxc":
        if kind == "bindless":
            return [exe, "-spirv", "-T", "cs_6_6", "-E", "main", "-O3", "-fspv-target-env=vulkan1.2", "-Fo", dst, src]
        cmd = [exe, "-spirv", "-T", "cs_6_2", "-E", "main", "-O3", "-fspv-target-env=vulkan1.1", "-Fo", dst, src]
        if kind == "original":
            cmd += shifts
        return cmd
    if compiler == "glslang":
        cmd = [exe, "-D", "-V", "-S", "comp", "-e", "main", "--target-env", "vulkan1.1", "-o", dst, src]
        if kind == "original":
            cmd += ["--shift-cbuffer-binding", "0", "--shift-UBO-binding", "0",
                    "--shift-texture-binding", "1", "--shift-ssbo-binding", "1",
                    "--shift-image-binding", "2", "--shift-uav-binding", "2",
                    "--shift-sampler-binding", "3"]
        return cmd
    raise ValueError(compiler)


def skip_reason(log, returncode):
    lines = [l.strip() for l in log.splitlines() if l.strip()]
    # A compiler that crashes prints nothing useful: say so.
    if not any(not l.lower().endswith((".hlsl", ".hlsli")) for l in lines):
        return f"compiler exited with code {returncode} without a message"
    line = next((l for l in lines if re.search(r"\berror\b", l, re.I)), lines[0] if lines else "no message")

    # Drop "<origin>: error <code>:" / "ERROR: <file>:<line>:" / "'#error' :" prefixes, then any leftover keyword.
    for _ in range(3):
        line = re.sub(r"^.*?\b(?:error|warning)\b[^:]*:\s*", "", line, flags=re.I)
        line = re.sub(r"^(?:[A-Za-z]:)?[^:\s]*:\d+:\s*", "", line)
    line = re.sub(r"\b(?:error|warning)s?\b", "problem", line, flags=re.I).strip().strip('"')

    return line[:100] or "no message"


# Warn if a shader using NonUniformResourceIndex lost NonUniform decorations.
def check_nonuniform(spv, name):
    dis = shutil.which("spirv-dis")
    if dis is None:
        return

    out = subprocess.run([dis, spv], capture_output=True, text=True).stdout
    decorated = set(re.findall(r"OpDecorate (%\S+) NonUniform\b", out))
    problem = None

    if not decorated:
        problem = "no NonUniform decoration in the SPIR-V"
    else:
        # Pointers derived from a decorated access chain must stay decorated down to the load.
        for result, op, base in re.findall(r"(%\S+) = (OpAccessChain|OpInBoundsAccessChain|OpLoad) %\S+ (%\S+)", out):
            if base in decorated and result not in decorated:
                problem = f"{op} {result} uses the NonUniform pointer {base} but is not decorated"
                break
    if problem:
        print(f"  WARNING {name}: {problem}. This compiler loses NonUniformResourceIndex:\n"
              f"  Check with --verify.")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)

    parser.add_argument("--compiler", choices=sorted(DEFAULT_EXE), required=True)
    parser.add_argument("--exe", help="compiler executable (default: found in PATH)")
    parser.add_argument("--out", default=os.path.join(ROOT, "build", "shaders"),
                        help="output root; SPIR-V goes to <out>/<compiler>/")
    parser.add_argument("--no-bindless", action="store_true", help="skip the bindless suite shaders (shaders/bindless)")
    parser.add_argument("--verbose", action="store_true", help="print full compiler logs for failures")

    args = parser.parse_args()

    exe = args.exe or DEFAULT_EXE[args.compiler]

    if shutil.which(exe) is None and not os.path.isfile(exe):
        print(f"error: compiler '{exe}' not found", file=sys.stderr)
        return 1

    out_dir = os.path.join(args.out, args.compiler)
    os.makedirs(out_dir, exist_ok=True)

    jobs = [(os.path.join(SHADER_DIR, f), "original") for f in sorted(os.listdir(SHADER_DIR)) if f.endswith(".hlsl")]

    if not args.no_bindless and os.path.isdir(BINDLESS_DIR):
        if args.compiler == "glslang":
            print("  skip bindless suite (glslang's HLSL front end lacks unbounded resource arrays and templated loads)")
        else:
            jobs += [(os.path.join(BINDLESS_DIR, f), "bindless") for f in sorted(os.listdir(BINDLESS_DIR)) if f.endswith(".hlsl")]

    failures = []
    for src, kind in jobs:

        optional = kind != "original"
        name = os.path.splitext(os.path.basename(src))[0]
        dst = os.path.join(out_dir, name + ".spv")
        cmd = command_for(args.compiler, exe, src, dst, kind)

        result = subprocess.run(cmd, cwd=os.path.dirname(src), capture_output=True, text=True)

        if result.returncode != 0 or not os.path.isfile(dst):
            failures.append(name)
            log = (result.stdout + result.stderr).strip()
            if optional and not args.verbose:
                print(f"  skip {name} (not supported by {args.compiler}: {skip_reason(log, result.returncode)})")
            else:
                print(f"  FAIL {name}\n{log}\n", file=sys.stderr)
        else:
            print(f"  ok   {name}")
            if "nonuniform" in name.lower() or "scalarized" in name.lower():
                check_nonuniform(dst, name)

    # Record which compiler produced the set, shown in the benchmark output.
    with open(os.path.join(out_dir, "compiler.txt"), "w") as f:
        try:
            version = subprocess.run([exe, "-v" if args.compiler == "slang" else "--version"],
                                     capture_output=True, text=True, timeout=30)
            lines = (version.stdout or version.stderr).strip().splitlines() if version.returncode == 0 else []
        except (OSError, subprocess.TimeoutExpired):
            lines = []

        f.write(f"{args.compiler} {lines[0].strip() if lines else os.path.basename(exe)}\n")

    print(f"\n{len(jobs) - len(failures)}/{len(jobs)} shaders compiled to {out_dir}")

    if failures:
        if all(n.startswith("bindless") for n in failures):
            print("only bindless shaders failed; their tests will be skipped at runtime")
            return 0
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
