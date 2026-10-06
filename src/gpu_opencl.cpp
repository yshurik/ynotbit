// The OpenCL backend: the driver is loaded at run time, so building needs no
// OpenCL SDK and a machine without one simply has no GPU here.
#include "gpu_backend.h"
#include <QLibrary>
#include <algorithm>
#include <cstdint>

// The few OpenCL 1.2 declarations used here.
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

const char *kPrefix = R"CL(
#define ROR(x, n) rotate((x), (ulong)(64 - (n)))
#define CONSTANT __constant
#define THREAD
)CL";
const char *kKernel = R"CL(
__kernel void trial(__constant ulong *initial, ulong base, uint perItem, ulong target,
                    __global ulong *found) {
    ulong start = base + (ulong)get_global_id(0) * perItem;
    for (uint k = 0; k < perItem; k++) {
        ulong nonce = start + k;
        if (trial_value(nonce, initial) <= target) {
            found[0] = nonce;
            found[1] = 1;
        }
    }
}
)CL";

class OpenCL final : public bm::gpu::Backend {
  public:
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
    cl_device_id device_ = nullptr;
    cl_context context = nullptr;
    cl_command_queue queue = nullptr;
    cl_program program = nullptr;
    cl_kernel kernel = nullptr;
    cl_mem initial = nullptr, found = nullptr;
    size_t global = 0;
    QString name;

    QString api() const override {
        return "OpenCL";
    }
    QString device() const override {
        return name;
    }
    quint64 width() const override {
        return global;
    }
    QString begin(const quint64 words[8]) override {
        const cl_ulong zero[2] = {0, 0};
        cl_int err =
            clEnqueueWriteBuffer(queue, initial, CL_TRUE, 0, 64, words, 0, nullptr, nullptr);
        if (err == CL_SUCCESS)
            err = clEnqueueWriteBuffer(queue, found, CL_TRUE, 0, 16, zero, 0, nullptr, nullptr);
        return err == CL_SUCCESS ? QString() : QString("OpenCL error %1").arg(err);
    }
    bm::gpu::Batch run(quint64 base, quint32 perItem, quint64 target) override {
        const cl_ulong b = base, t = target;
        const cl_uint n = perItem;
        clSetKernelArg(kernel, 0, sizeof(cl_mem), &initial);
        clSetKernelArg(kernel, 1, sizeof b, &b);
        clSetKernelArg(kernel, 2, sizeof n, &n);
        clSetKernelArg(kernel, 3, sizeof t, &t);
        clSetKernelArg(kernel, 4, sizeof(cl_mem), &found);
        cl_int err = clEnqueueNDRangeKernel(queue, kernel, 1, nullptr, &global, nullptr, 0, nullptr,
                                            nullptr);
        cl_ulong result[2] = {0, 0};
        if (err == CL_SUCCESS)
            err = clEnqueueReadBuffer(queue, found, CL_TRUE, 0, 16, result, 0, nullptr, nullptr);
        if (err != CL_SUCCESS)
            return {std::nullopt, QString("OpenCL error %1").arg(err)};
        if (result[1])
            return {result[0], {}};
        return {};
    }
};
} // namespace

namespace bm::gpu {
std::unique_ptr<Backend> openOpenCL(QString *problem) {
    auto s = std::make_unique<OpenCL>();
#if defined(Q_OS_MACOS)
    s->library.setFileName("/System/Library/Frameworks/OpenCL.framework/OpenCL");
#elif defined(Q_OS_WIN)
    s->library.setFileName("OpenCL");
#else
    s->library.setFileNameAndVersion("OpenCL", 1);
#endif
    if (!s->library.load()) {
        *problem = "no OpenCL driver";
        return nullptr;
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
        *problem = "incomplete OpenCL driver";
        return nullptr;
    }
    // The GPU with the most compute units, on any platform.
    cl_platform_id platforms[8];
    cl_uint platformCount = 0;
    if (s->clGetPlatformIDs(8, platforms, &platformCount) != CL_SUCCESS || !platformCount) {
        *problem = "no OpenCL platform";
        return nullptr;
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
                s->device_ = devices[d];
            }
        }
    }
    if (!s->device_) {
        *problem = "no OpenCL GPU";
        return nullptr;
    }
    char name[256] = {};
    s->clGetDeviceInfo(s->device_, CL_DEVICE_NAME, sizeof name - 1, name, nullptr);
    const QByteArray source = QByteArray(kPrefix) + kTrialSource + kKernel;
    const char *text = source.constData();
    cl_int err = CL_SUCCESS;
    s->context = s->clCreateContext(nullptr, 1, &s->device_, nullptr, nullptr, &err);
    if (err == CL_SUCCESS)
        s->queue = s->clCreateCommandQueue(s->context, s->device_, 0, &err);
    if (err == CL_SUCCESS)
        s->program = s->clCreateProgramWithSource(s->context, 1, &text, nullptr, &err);
    if (err == CL_SUCCESS &&
        s->clBuildProgram(s->program, 1, &s->device_, "", nullptr, nullptr) != CL_SUCCESS) {
        char log[2048] = {};
        s->clGetProgramBuildInfo(s->program, s->device_, CL_PROGRAM_BUILD_LOG, sizeof log - 1, log,
                                 nullptr);
        *problem = "GPU kernel did not compile: " + QString::fromLocal8Bit(log).left(300);
        return nullptr;
    }
    if (err == CL_SUCCESS)
        s->kernel = s->clCreateKernel(s->program, "trial", &err);
    if (err == CL_SUCCESS)
        s->initial = s->clCreateBuffer(s->context, CL_MEM_READ_ONLY, 64, nullptr, &err);
    if (err == CL_SUCCESS)
        s->found = s->clCreateBuffer(s->context, CL_MEM_READ_WRITE, 16, nullptr, &err);
    if (err != CL_SUCCESS) {
        *problem = QString("OpenCL setup failed (%1)").arg(err);
        return nullptr;
    }
    s->global = size_t(std::max<cl_uint>(bestUnits, 1)) * 2048;
    s->name = QString::fromLocal8Bit(name).trimmed();
    return s;
}
} // namespace bm::gpu
