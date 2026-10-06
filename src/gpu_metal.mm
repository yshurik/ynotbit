// The Metal backend: Apple's own GPU interface, which replaces the OpenCL
// Apple deprecated. The kernel is the code OpenCL runs, compiled from source
// when first used, so neither Xcode's shader compiler nor a library is needed.
#include "gpu_backend.h"
#include <IOKit/IOKitLib.h>
#import <Metal/Metal.h>
#include <cstring>

namespace {
// Metal's built-in rotate measured a fifth faster here than two shifts.
const char *kPrefix = R"MSL(
#include <metal_stdlib>
using namespace metal;
#define ROR(x, n) rotate((x), (ulong)(64 - (n)))
#define CONSTANT constant
#define THREAD thread
)MSL";
const char *kKernel = R"MSL(
kernel void trial(constant ulong *initial [[buffer(0)]], constant ulong &base [[buffer(1)]],
                  constant uint &perItem [[buffer(2)]], constant ulong &target [[buffer(3)]],
                  device ulong *found [[buffer(4)]], uint item [[thread_position_in_grid]]) {
    ulong start = base + (ulong)item * perItem;
    for (uint k = 0; k < perItem; k++) {
        ulong nonce = start + k;
        if (trial_value(nonce, initial) <= target) {
            found[0] = nonce;
            found[1] = 1;
        }
    }
}
)MSL";
// An Apple GPU's core count, which Metal does not report; 0 when unknown.
NSUInteger appleGpuCores() {
    NSUInteger cores = 0;
    const io_service_t gpu =
        IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("AGXAccelerator"));
    if (!gpu)
        return 0;
    if (const CFTypeRef value =
            IORegistryEntryCreateCFProperty(gpu, CFSTR("gpu-core-count"), kCFAllocatorDefault, 0)) {
        long long count = 0;
        if (CFGetTypeID(value) == CFNumberGetTypeID() &&
            CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberLongLongType, &count) &&
            count > 0)
            cores = NSUInteger(count);
        CFRelease(value);
    }
    IOObjectRelease(gpu);
    return cores;
}

class Metal final : public bm::gpu::Backend {
  public:
    id<MTLDevice> gpu;
    id<MTLCommandQueue> queue;
    id<MTLComputePipelineState> pipeline;
    id<MTLBuffer> initial, found;
    // One SIMD group per threadgroup, measured fastest for this register-heavy
    // kernel; 2048 work items per GPU core, as OpenCL sizes it.
    NSUInteger group = 32, items = 16384;
    QString name;

    QString api() const override {
        return "Metal";
    }
    QString device() const override {
        return name;
    }
    quint64 width() const override {
        return items;
    }
    QString begin(const quint64 words[8]) override {
        std::memcpy(initial.contents, words, 64);
        std::memset(found.contents, 0, 16);
        return {};
    }
    bm::gpu::Batch run(quint64 base, quint32 perItem, quint64 target) override {
        @autoreleasepool {
            id<MTLCommandBuffer> commands = [queue commandBuffer];
            id<MTLComputeCommandEncoder> encoder = [commands computeCommandEncoder];
            [encoder setComputePipelineState:pipeline];
            [encoder setBuffer:initial offset:0 atIndex:0];
            [encoder setBytes:&base length:sizeof base atIndex:1];
            [encoder setBytes:&perItem length:sizeof perItem atIndex:2];
            [encoder setBytes:&target length:sizeof target atIndex:3];
            [encoder setBuffer:found offset:0 atIndex:4];
            [encoder dispatchThreadgroups:MTLSizeMake(items / group, 1, 1)
                    threadsPerThreadgroup:MTLSizeMake(group, 1, 1)];
            [encoder endEncoding];
            [commands commit];
            [commands waitUntilCompleted];
            if (commands.status != MTLCommandBufferStatusCompleted)
                return {std::nullopt,
                        "Metal: " + QString::fromNSString(commands.error.localizedDescription)};
            const auto *result = static_cast<const quint64 *>(found.contents);
            if (result[1])
                return {result[0], {}};
            return {};
        }
    }
};
} // namespace

namespace bm::gpu {
std::unique_ptr<Backend> openMetal(QString *problem) {
    @autoreleasepool {
        auto s = std::make_unique<Metal>();
        s->gpu = MTLCreateSystemDefaultDevice();
        if (!s->gpu) {
            *problem = "no Metal GPU";
            return nullptr;
        }
        const QByteArray source = QByteArray(kPrefix) + kTrialSource + kKernel;
        NSError *error = nil;
        id<MTLLibrary> library =
            [s->gpu newLibraryWithSource:[NSString stringWithUTF8String:source.constData()]
                                 options:nil
                                   error:&error];
        if (!library) {
            *problem = "GPU kernel did not compile: " +
                       QString::fromNSString(error.localizedDescription).left(300);
            return nullptr;
        }
        id<MTLFunction> function = [library newFunctionWithName:@"trial"];
        s->pipeline =
            function ? [s->gpu newComputePipelineStateWithFunction:function error:&error] : nil;
        s->queue = [s->gpu newCommandQueue];
        s->initial = [s->gpu newBufferWithLength:64 options:MTLResourceStorageModeShared];
        s->found = [s->gpu newBufferWithLength:16 options:MTLResourceStorageModeShared];
        if (!s->pipeline || !s->queue || !s->initial || !s->found) {
            *problem = "Metal setup failed" +
                       (error ? ": " + QString::fromNSString(error.localizedDescription).left(300)
                              : QString());
            return nullptr;
        }
        s->group = s->pipeline.threadExecutionWidth;
        if (const NSUInteger cores = appleGpuCores())
            s->items = cores * 2048;
        s->items = (s->items + s->group - 1) / s->group * s->group;
        s->name = QString::fromNSString(s->gpu.name);
        return s;
    }
}
} // namespace bm::gpu
