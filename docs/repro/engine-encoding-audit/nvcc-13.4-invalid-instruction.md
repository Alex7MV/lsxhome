# nvcc 13.4: `parse Invalid instruction with no BB (Producer: 'LLVM23.0.0')`
# Reproducer material for a forum report.
#
# WHAT FAILS
#   Compiling libs/lsxcommon/src/xing_moe_inference.cu fails with
#     parse Invalid instruction with no BB (Producer: 'LLVM23.0.0')
#   nvcc exits with code 1, no diagnostic pointing at a source line.
#
# THE EXACT COMMAND (verbatim from the failing build)
#   "C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.4\bin\nvcc.exe"
#     --use-local-env
#     -ccbin "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231\bin\HostX64\x64"
#     -x cu
#     -I<logestix>/libs/lsxcommon/src
#     -I<logestix>/libs/lsxcommon/include
#     -I<logestix>/third_party/cccl/thrust
#     -I<logestix>/third_party/cccl/cub
#     -I<logestix>/third_party/cccl/libcudacxx/include
#     -I<build>/libs/lsxcommon/include
#     -I<arrow>/cpp/src -I<arrow-build>/src
#     -I<spdlog>/include -I<abseil>
#     -I"C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.4\include"
#     -I"...\CUDA\v13.4\include\cccl"
#     -maxrregcount=0
#     --machine 64 --compile -forward-unknown-to-host-compiler
#     -std=c++20
#     --generate-code=arch=compute_120,code=[sm_120]
#     --generate-code=arch=compute_121,code=[sm_121]
#     --extended-lambda
#     --expt-relaxed-constexpr
#     -fmad=false
#     -Xcompiler="/EHsc -Ob0 -Zi /utf-8" -g
#     -D_WINDOWS -DTHRUST_DEVICE_SYSTEM=THRUST_DEVICE_SYSTEM_CPP
#     -DCCCL_IGNORE_MSVC_TRADITIONAL_PREPROCESSOR_WARNING -DLOGESTIX_HAS_CUDA
#     -Xcompiler "/EHsc /W4 /nologo /Od /FS /Zi /RTC1 /MDd "
#     -c xing_moe_inference.cu
#
# ENVIRONMENT
#   Windows 11, x64
#   CUDA Toolkit 13.4 (nvcc V13.4.92), driver reports 13.x
#   MSVC 14.51.36231 (Visual Studio 2026), /MDd → _DEBUG defined
#   CCCL 3.4.2 (thrust / cub / libcu++), sm_120 + sm_121
#
# MEASURED TRIGGER MATRIX (same file, same flags otherwise)
#
#   host configuration                        --expt-relaxed-constexpr   result
#   ---------------------------------------  -------------------------  ----------------
#   Debug   (/Od /Zi /RTC1 /MDd, _DEBUG)     present                    FAILS
#   Debug   (/Od /Zi /RTC1 /MDd, _DEBUG)     removed                    compiles
#   Release (/O2, no _DEBUG)                 present                    compiles
#   Debug + present, -fmad=false removed                 still fails (not involved)
#
#   => the failure needs BOTH --expt-relaxed-constexpr AND a Debug host
#      configuration (/MDd, i.e. _DEBUG). Either one alone is enough to avoid it.
#
# LOCALISATION ATTEMPTS (all negative, i.e. the failure needs the TU body)
#   * Every include of xing_moe_inference.cu compiled standalone with the same
#     flags compiles clean: xing_moe_constants.h, xing_moe_device_math.h,
#     lsx_kernels_act.cuh, lsx_kernels_math.cuh, lsx_kernels_norm.cuh,
#     lsxcommon/xing_moe_kernels.h, lsxcommon/lsx_inference_nvfp4.h,
#     lsxcommon/lsx_gemv.h, lsxcommon/shared_experts.h.
#   * Disabling the whole body (everything after the last `namespace`) makes the
#     error disappear, so the trigger lives in the translation unit's own code,
#     not in a header.
#   * Standalone candidates that all COMPILE CLEAN with the failing flags
#     (so none of these is the trigger on its own):
#       - constexpr as an index / loop bound / __shared__ array extent
#       - constexpr as a template argument (device function)
#       - constexpr float/double arithmetic inside a kernel
#       - constexpr member (static constexpr float) used in a kernel
#       - constexpr used as __shfl_down_sync offset
#       - constexpr std::array initialiser read in device code
#       - thrust::device_ptr arithmetic with constexpr
#       - thrust::reduce on a device_vector with constexpr
#       - cub::BlockReduce with a constexpr block size
#
# QUESTIONS
#   1. Is `--expt-relaxed-constexpr` expected to work in a Debug build
#      (/MDd, _DEBUG defined)? The same TU compiles in Release with the flag.
#   2. The crash carries no source location. Is there a flag (e.g. `-Xptxas -v`,
#      `--keep`, `-Xcicc -print-after-all`, or setting NVVM IR verification) that
#      makes nvcc report WHICH function it fails in, so the offending construct
#      can be reduced to a minimal reproducer?
#   3. Known issue with NVVM + `--expt-relaxed-constexpr` + `-G`/`_DEBUG` in
#      CUDA 13.x?
#
# NOTE
#   Two other translation units of the same project fail identically
#   (perf_calibration.cu, gigachat_moe_inference_pipeline.cu), which suggests a
#   common ingredient rather than one pathological kernel.
#
# UPDATE (engine pin 879e422)
#   The engine now guards the flag out of Debug:
#     $<$<COMPILE_LANGUAGE:CUDA>:--extended-lambda;$<$<NOT:$<CONFIG:Debug>>:--expt-relaxed-constexpr>>
#   so in Debug nvcc no longer receives --expt-relaxed-constexpr. The NVVM abort
#   disappears — and the build breaks the other way instead:
#
#     gemma_dense_inference.cu: error: calling a constexpr __host__ function
#       ("Offset") from a __global__ function ("gemma_dense_attn_kernel") is
#       not allowed. The experimental flag '--expt-relaxed-constexpr' can be
#       used to allow this ...
#     (also xing_moe_kernels.cu and xing_moe_inference.cu)
#
#   So Debug is blocked either way on CUDA 13.4:
#     * flag present -> NVVM: parse Invalid instruction with no BB;
#     * flag absent  -> nvcc: constexpr __host__ call from __global__.
#   A permanent fix needs a flag that compiles the `constexpr __host__ Offset`
#   calls correctly AND does not abort NVVM in a /MDd configuration. Release
#   builds are unaffected.