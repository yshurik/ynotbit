#include "gpu_pow.h"
#include <QElapsedTimer>
#include <QLibrary>
#include <QtEndian>
#include <cstdint>
#include <vector>

// The few OpenCL 1.2 declarations used here, so building needs no OpenCL SDK.
#ifdef _WIN32
#define CL_API_CALL __stdcall
#else
#define CL_API_CALL
#endif
namespace {
using cl_int = int32_t;
using cl_uint = uint32_t;
using cl_ulong = uint64_t;
using cl_bitfield = uint64_t;
using cl_platform_id = struct _cl_platform_id *;
using cl_device_id = struct _cl_device_id *;
using cl_context = struct _cl_context *;
using cl_command_queue = struct _cl_command_queue *;
using cl_program = struct _cl_program *;
using cl_kernel = struct _cl_kernel *;
using cl_mem = struct _cl_mem *;
constexpr cl_int CL_SUCCESS = 0;
constexpr cl_uint CL_TRUE = 1;
constexpr cl_bitfield CL_DEVICE_TYPE_GPU = 1 << 2;
constexpr cl_bitfield CL_MEM_READ_WRITE = 1 << 0;
constexpr cl_bitfield CL_MEM_READ_ONLY = 1 << 2;
constexpr cl_uint CL_DEVICE_MAX_COMPUTE_UNITS = 0x1002;
constexpr cl_uint CL_DEVICE_NAME = 0x102B;
constexpr cl_uint CL_PROGRAM_BUILD_LOG = 0x1183;

// Bitmessage's trial value: the first 8 bytes, big-endian, of
// SHA512(SHA512(nonce || initial)). Both messages fit one SHA-512 block.
const char *kKernel = R"CL(
#define ROR(x, n) rotate((x), (ulong)(64 - (n)))
__constant ulong K[80] = {
0x428a2f98d728ae22UL, 0x7137449123ef65cdUL, 0xb5c0fbcfec4d3b2fUL, 0xe9b5dba58189dbbcUL,
0x3956c25bf348b538UL, 0x59f111f1b605d019UL, 0x923f82a4af194f9bUL, 0xab1c5ed5da6d8118UL,
0xd807aa98a3030242UL, 0x12835b0145706fbeUL, 0x243185be4ee4b28cUL, 0x550c7dc3d5ffb4e2UL,
0x72be5d74f27b896fUL, 0x80deb1fe3b1696b1UL, 0x9bdc06a725c71235UL, 0xc19bf174cf692694UL,
0xe49b69c19ef14ad2UL, 0xefbe4786384f25e3UL, 0x0fc19dc68b8cd5b5UL, 0x240ca1cc77ac9c65UL,
0x2de92c6f592b0275UL, 0x4a7484aa6ea6e483UL, 0x5cb0a9dcbd41fbd4UL, 0x76f988da831153b5UL,
0x983e5152ee66dfabUL, 0xa831c66d2db43210UL, 0xb00327c898fb213fUL, 0xbf597fc7beef0ee4UL,
0xc6e00bf33da88fc2UL, 0xd5a79147930aa725UL, 0x06ca6351e003826fUL, 0x142929670a0e6e70UL,
0x27b70a8546d22ffcUL, 0x2e1b21385c26c926UL, 0x4d2c6dfc5ac42aedUL, 0x53380d139d95b3dfUL,
0x650a73548baf63deUL, 0x766a0abb3c77b2a8UL, 0x81c2c92e47edaee6UL, 0x92722c851482353bUL,
0xa2bfe8a14cf10364UL, 0xa81a664bbc423001UL, 0xc24b8b70d0f89791UL, 0xc76c51a30654be30UL,
0xd192e819d6ef5218UL, 0xd69906245565a910UL, 0xf40e35855771202aUL, 0x106aa07032bbd1b8UL,
0x19a4c116b8d2d0c8UL, 0x1e376c085141ab53UL, 0x2748774cdf8eeb99UL, 0x34b0bcb5e19b48a8UL,
0x391c0cb3c5c95a63UL, 0x4ed8aa4ae3418acbUL, 0x5b9cca4f7763e373UL, 0x682e6ff3d6b2b8a3UL,
0x748f82ee5defb2fcUL, 0x78a5636f43172f60UL, 0x84c87814a1f0ab72UL, 0x8cc702081a6439ecUL,
0x90befffa23631e28UL, 0xa4506cebde82bde9UL, 0xbef9a3f7b2c67915UL, 0xc67178f2e372532bUL,
0xca273eceea26619cUL, 0xd186b8c721c0c207UL, 0xeada7dd6cde0eb1eUL, 0xf57d4f7fee6ed178UL,
0x06f067aa72176fbaUL, 0x0a637dc5a2c898a6UL, 0x113f9804bef90daeUL, 0x1b710b35131c471bUL,
0x28db77f523047d84UL, 0x32caab7b40c72493UL, 0x3c9ebe0a15c9bebcUL, 0x431d67c49c100d4cUL,
0x4cc5d4becb3e42b6UL, 0x597f299cfc657e2aUL, 0x5fcb6fab3ad6faecUL, 0x6c44198c4a475817UL};
__constant ulong H0[8] = {
0x6a09e667f3bcc908UL, 0xbb67ae8584caa73bUL, 0x3c6ef372fe94f82bUL, 0xa54ff53a5f1d36f1UL,
0x510e527fade682d1UL, 0x9b05688c2b3e6c1fUL, 0x1f83d9abfb41bd6bUL, 0x5be0cd19137e2179UL};
static void compress(ulong *h, ulong *w) {
    for (int i = 16; i < 80; i++) {
        ulong s0 = ROR(w[i - 15], 1) ^ ROR(w[i - 15], 8) ^ (w[i - 15] >> 7);
        ulong s1 = ROR(w[i - 2], 19) ^ ROR(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    ulong a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 80; i++) {
        ulong t1 = hh + (ROR(e, 14) ^ ROR(e, 18) ^ ROR(e, 41)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        ulong t2 = (ROR(a, 28) ^ ROR(a, 34) ^ ROR(a, 39)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}
__kernel void trial(__constant ulong *initial, ulong base, uint perItem, ulong target,
                    __global ulong *found) {
    ulong w[80], h[8];
    ulong start = base + (ulong)get_global_id(0) * perItem;
    for (uint k = 0; k < perItem; k++) {
        ulong nonce = start + k;
        w[0] = nonce;
        for (int i = 0; i < 8; i++) w[1 + i] = initial[i];
        w[9] = 0x8000000000000000UL;
        for (int i = 10; i < 15; i++) w[i] = 0;
        w[15] = 72 * 8;
        for (int i = 0; i < 8; i++) h[i] = H0[i];
        compress(h, w);
        for (int i = 0; i < 8; i++) w[i] = h[i];
        w[8] = 0x8000000000000000UL;
        for (int i = 9; i < 15; i++) w[i] = 0;
        w[15] = 64 * 8;
        for (int i = 0; i < 8; i++) h[i] = H0[i];
        compress(h, w);
        if (h[0] <= target) {
            found[0] = nonce;
            found[1] = 1;
        }
    }
}
)CL";
} // namespace

namespace bm {
struct GpuSolver::State {
    QLibrary library;
#define CL_FN(name, ret, ...) ret(CL_API_CALL *name)(__VA_ARGS__) = nullptr
    CL_FN(clGetPlatformIDs, cl_int, cl_uint, cl_platform_id *, cl_uint *);
    CL_FN(clGetDeviceIDs, cl_int, cl_platform_id, cl_bitfield, cl_uint, cl_device_id *, cl_uint *);
    CL_FN(clGetDeviceInfo, cl_int, cl_device_id, cl_uint, size_t, void *, size_t *);
    CL_FN(clCreateContext, cl_context, const intptr_t *, cl_uint, const cl_device_id *, void *,
          void *, cl_int *);
    CL_FN(clCreateCommandQueue, cl_command_queue, cl_context, cl_device_id, cl_bitfield, cl_int *);
    CL_FN(clCreateProgramWithSource, cl_program, cl_context, cl_uint, const char **, const size_t *,
          cl_int *);
    CL_FN(clBuildProgram, cl_int, cl_program, cl_uint, const cl_device_id *, const char *, void *,
          void *);
    CL_FN(clGetProgramBuildInfo, cl_int, cl_program, cl_device_id, cl_uint, size_t, void *,
          size_t *);
    CL_FN(clCreateKernel, cl_kernel, cl_program, const char *, cl_int *);
    CL_FN(clCreateBuffer, cl_mem, cl_context, cl_bitfield, size_t, void *, cl_int *);
    CL_FN(clSetKernelArg, cl_int, cl_kernel, cl_uint, size_t, const void *);
    CL_FN(clEnqueueWriteBuffer, cl_int, cl_command_queue, cl_mem, cl_uint, size_t, size_t,
          const void *, cl_uint, const void *, void *);
    CL_FN(clEnqueueReadBuffer, cl_int, cl_command_queue, cl_mem, cl_uint, size_t, size_t, void *,
          cl_uint, const void *, void *);
    CL_FN(clEnqueueNDRangeKernel, cl_int, cl_command_queue, cl_kernel, cl_uint, const size_t *,
          const size_t *, const size_t *, cl_uint, const void *, void *);
#undef CL_FN
    cl_device_id device = nullptr;
    cl_context context = nullptr;
    cl_command_queue queue = nullptr;
    cl_program program = nullptr;
    cl_kernel kernel = nullptr;
    cl_mem initial = nullptr, found = nullptr;
    size_t global = 0;
    cl_uint perItem = 16; // tuned per batch towards kBatchMs
};

GpuSolver &GpuSolver::instance() {
    // Never destroyed: the driver may already be gone when statics are torn down.
    static auto *solver = new GpuSolver;
    return *solver;
}
GpuSolver::~GpuSolver() = default;

void GpuSolver::open() {
    if (qEnvironmentVariableIsSet("YNOTBIT_NO_GPU")) {
        problem_ = "switched off by YNOTBIT_NO_GPU";
        return;
    }
    auto s = std::make_unique<State>();
#if defined(Q_OS_MACOS)
    s->library.setFileName("/System/Library/Frameworks/OpenCL.framework/OpenCL");
#elif defined(Q_OS_WIN)
    s->library.setFileName("OpenCL");
#else
    s->library.setFileNameAndVersion("OpenCL", 1);
#endif
    if (!s->library.load()) {
        problem_ = "no OpenCL driver";
        return;
    }
    bool resolved = true;
    const auto resolve = [&](auto &fn, const char *name) {
        fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(s->library.resolve(name));
        resolved = resolved && fn;
    };
    resolve(s->clGetPlatformIDs, "clGetPlatformIDs");
    resolve(s->clGetDeviceIDs, "clGetDeviceIDs");
    resolve(s->clGetDeviceInfo, "clGetDeviceInfo");
    resolve(s->clCreateContext, "clCreateContext");
    resolve(s->clCreateCommandQueue, "clCreateCommandQueue");
    resolve(s->clCreateProgramWithSource, "clCreateProgramWithSource");
    resolve(s->clBuildProgram, "clBuildProgram");
    resolve(s->clGetProgramBuildInfo, "clGetProgramBuildInfo");
    resolve(s->clCreateKernel, "clCreateKernel");
    resolve(s->clCreateBuffer, "clCreateBuffer");
    resolve(s->clSetKernelArg, "clSetKernelArg");
    resolve(s->clEnqueueWriteBuffer, "clEnqueueWriteBuffer");
    resolve(s->clEnqueueReadBuffer, "clEnqueueReadBuffer");
    resolve(s->clEnqueueNDRangeKernel, "clEnqueueNDRangeKernel");
    if (!resolved) {
        problem_ = "incomplete OpenCL driver";
        return;
    }
    // The GPU with the most compute units, on any platform.
    cl_platform_id platforms[8];
    cl_uint platformCount = 0;
    if (s->clGetPlatformIDs(8, platforms, &platformCount) != CL_SUCCESS || !platformCount) {
        problem_ = "no OpenCL platform";
        return;
    }
    cl_uint bestUnits = 0;
    for (cl_uint p = 0; p < std::min<cl_uint>(platformCount, 8); ++p) {
        cl_device_id devices[8];
        cl_uint count = 0;
        if (s->clGetDeviceIDs(platforms[p], CL_DEVICE_TYPE_GPU, 8, devices, &count) != CL_SUCCESS)
            continue;
        for (cl_uint d = 0; d < std::min<cl_uint>(count, 8); ++d) {
            cl_uint units = 0;
            s->clGetDeviceInfo(devices[d], CL_DEVICE_MAX_COMPUTE_UNITS, sizeof units, &units,
                               nullptr);
            if (units > bestUnits) {
                bestUnits = units;
                s->device = devices[d];
            }
        }
    }
    if (!s->device) {
        problem_ = "no OpenCL GPU";
        return;
    }
    char name[256] = {};
    s->clGetDeviceInfo(s->device, CL_DEVICE_NAME, sizeof name - 1, name, nullptr);
    cl_int err = CL_SUCCESS;
    s->context = s->clCreateContext(nullptr, 1, &s->device, nullptr, nullptr, &err);
    if (err == CL_SUCCESS)
        s->queue = s->clCreateCommandQueue(s->context, s->device, 0, &err);
    if (err == CL_SUCCESS)
        s->program = s->clCreateProgramWithSource(s->context, 1, &kKernel, nullptr, &err);
    if (err == CL_SUCCESS &&
        s->clBuildProgram(s->program, 1, &s->device, "", nullptr, nullptr) != CL_SUCCESS) {
        char log[2048] = {};
        s->clGetProgramBuildInfo(s->program, s->device, CL_PROGRAM_BUILD_LOG, sizeof log - 1, log,
                                 nullptr);
        problem_ = "GPU kernel did not compile: " + QString::fromLocal8Bit(log).left(300);
        return;
    }
    if (err == CL_SUCCESS)
        s->kernel = s->clCreateKernel(s->program, "trial", &err);
    if (err == CL_SUCCESS)
        s->initial = s->clCreateBuffer(s->context, CL_MEM_READ_ONLY, 64, nullptr, &err);
    if (err == CL_SUCCESS)
        s->found = s->clCreateBuffer(s->context, CL_MEM_READ_WRITE, 16, nullptr, &err);
    if (err != CL_SUCCESS) {
        problem_ = QString("OpenCL setup failed (%1)").arg(err);
        return;
    }
    s->global = size_t(std::max<cl_uint>(bestUnits, 1)) * 2048;
    device_ = QString::fromLocal8Bit(name).trimmed();
    s_ = std::move(s);
    ok_ = true;
}
bool GpuSolver::available() {
    std::call_once(opened_, [this] { open(); });
    return ok_;
}
QString GpuSolver::problem() {
    available();
    std::lock_guard lock(use_);
    return problem_;
}
QString GpuSolver::deviceName() {
    available();
    std::lock_guard lock(use_);
    return device_;
}
void GpuSolver::disable(const QString &why) {
    std::lock_guard lock(use_);
    ok_ = false;
    problem_ = why;
}
std::optional<quint64> GpuSolver::search(const unsigned char initial[64], quint64 target,
                                         quint64 first, const std::atomic_bool &cancel) {
    if (!available())
        return std::nullopt;
    std::lock_guard lock(use_);
    if (!ok_)
        return std::nullopt;
    auto &s = *s_;
    cl_ulong words[8];
    for (int i = 0; i < 8; ++i)
        words[i] = qFromBigEndian<quint64>(initial + 8 * i);
    const cl_ulong zero[2] = {0, 0};
    const auto failed = [&](cl_int err) {
        ok_ = false;
        problem_ = QString("GPU stopped working (%1)").arg(err);
        return std::nullopt;
    };
    cl_int err =
        s.clEnqueueWriteBuffer(s.queue, s.initial, CL_TRUE, 0, 64, words, 0, nullptr, nullptr);
    if (err == CL_SUCCESS)
        err = s.clEnqueueWriteBuffer(s.queue, s.found, CL_TRUE, 0, 16, zero, 0, nullptr, nullptr);
    if (err != CL_SUCCESS)
        return failed(err);
    // Short batches keep cancelling quick and stay far below the time after
    // which Windows resets a GPU that does not answer (2 s).
    constexpr qint64 kBatchMs = 30;
    cl_ulong base = first;
    QElapsedTimer batch;
    while (!cancel) {
        const cl_ulong t = target;
        const cl_uint perItem = s.perItem;
        s.clSetKernelArg(s.kernel, 0, sizeof(cl_mem), &s.initial);
        s.clSetKernelArg(s.kernel, 1, sizeof base, &base);
        s.clSetKernelArg(s.kernel, 2, sizeof perItem, &perItem);
        s.clSetKernelArg(s.kernel, 3, sizeof t, &t);
        s.clSetKernelArg(s.kernel, 4, sizeof(cl_mem), &s.found);
        batch.start();
        err = s.clEnqueueNDRangeKernel(s.queue, s.kernel, 1, nullptr, &s.global, nullptr, 0,
                                       nullptr, nullptr);
        cl_ulong result[2] = {0, 0};
        if (err == CL_SUCCESS)
            err = s.clEnqueueReadBuffer(s.queue, s.found, CL_TRUE, 0, 16, result, 0, nullptr,
                                        nullptr);
        if (err != CL_SUCCESS)
            return failed(err);
        if (result[1])
            return result[0];
        base += cl_ulong(s.global) * perItem;
        const qint64 ms = batch.elapsed();
        if (ms < kBatchMs / 2 && s.perItem < 65536)
            s.perItem *= 2;
        else if (ms > kBatchMs * 2 && s.perItem > 1)
            s.perItem /= 2;
    }
    return std::nullopt;
}
} // namespace bm
