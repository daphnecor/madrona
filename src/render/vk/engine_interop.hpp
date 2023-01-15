#pragma once

#include <utility>

#include "memory.hpp"
#include "cuda_interop.hpp"
#include "utils.hpp"

namespace madrona::render::vk {

struct CpuMode {};
struct CudaMode {};

template <typename CudaT, typename CpuT>
struct EngineModeVariant {
    using BaseT = EngineModeVariant<CudaT, CpuT>;

    union {
        CudaT cuda;
        CpuT cpu;
    };

    bool isCuda;

    template <typename ...Args>
    inline EngineModeVariant(CudaMode, Args && ...args)
        : cuda(std::forward<Args>(args) ...),
          isCuda(true)
    {}

    template <typename ...Args>
    inline EngineModeVariant(CpuMode, Args && ...args)
        : cpu(std::forward<Args>(args) ...),
          isCuda(false)
    {}

    inline ~EngineModeVariant()
    {
        if (isCuda) {
            cuda.~CudaT();
        } else {
            cpu.~CpuT();
        }
    }

    inline EngineModeVariant(EngineModeVariant &&o)
    {
        isCuda = o.isCuda;

        if (isCuda) {
            new (&cuda) CudaT(std::move(o.cuda));
        } else {
            new (&cpu) CpuT(std::move(o.cpu));
        }
    }
};

struct HostToEngineBufferCUDA {
    HostBuffer staging;
    DedicatedBuffer devBuffer;
    CudaImportedBuffer cudaImported;

    inline HostToEngineBufferCUDA(const DeviceState &dev,
                                  MemoryAllocator &mem,
                                  uint64_t num_bytes,
                                  int cuda_gpu_id)
        : staging(mem.makeStagingBuffer(num_bytes)),
          devBuffer(mem.makeDedicatedBuffer(num_bytes)),
          cudaImported(dev, cuda_gpu_id, devBuffer.mem, num_bytes)
    {}
};

struct HostToEngineBufferCPU {
    void *ptr;
    uint64_t numBytes;

    inline HostToEngineBufferCPU(uint64_t num_bytes)
        : ptr(malloc(num_bytes)),
          numBytes(num_bytes)
    {}

    inline ~HostToEngineBufferCPU()
    {
        free(ptr);
    }

    inline HostToEngineBufferCPU(HostToEngineBufferCPU &&o)
        : ptr(o.ptr),
          numBytes(o.numBytes)
    {
        o.ptr = nullptr;
        o.numBytes = 0;
    }
};

struct HostToEngineBuffer : public EngineModeVariant<
        HostToEngineBufferCUDA, HostToEngineBufferCPU> {
    using BaseT::BaseT;

    inline void * enginePointer() const
    {
        if (isCuda) {
            return cuda.cudaImported.getDevicePointer();
        } else {
            return cpu.ptr;
        }
    }

    inline void * hostPointer() const
    {
        if (isCuda) {
            return cuda.staging.ptr;
        } else {
            return cpu.ptr;
        }
    }

    inline bool needsEngineCopy() const
    {
        return isCuda;
    }

    inline void toEngine(const DeviceState &dev, VkCommandBuffer cmd,
                         uint32_t offset, uint32_t num_bytes)
    {
        if (isCuda) {
            cuda.staging.flush(dev);

            VkBufferCopy buffer_copy;
            buffer_copy.srcOffset = offset;
            buffer_copy.dstOffset = offset;
            buffer_copy.size = num_bytes;

            dev.dt.cmdCopyBuffer(cmd, cuda.staging.buffer,
                                 cuda.devBuffer.buf.buffer, 1,
                                 &buffer_copy);
        }
    }
};

struct EngineToRendererBufferCUDA {
    DedicatedBuffer devBuffer;
    CudaImportedBuffer cudaImported;

    inline EngineToRendererBufferCUDA(const DeviceState &dev,
            MemoryAllocator &mem, uint64_t num_bytes, int cuda_gpu_id)
        : devBuffer(mem.makeDedicatedBuffer(num_bytes, true)),
          cudaImported(dev, cuda_gpu_id, devBuffer.mem, num_bytes)
    {}
};

struct EngineToRendererBufferCPU {
    HostBuffer staging;
    DedicatedBuffer devBuffer;
    uint64_t numBytes;

    inline EngineToRendererBufferCPU(MemoryAllocator &mem, uint64_t num_bytes)
        : staging(mem.makeStagingBuffer(num_bytes)),
          devBuffer(mem.makeDedicatedBuffer(num_bytes, true)),
          numBytes(num_bytes)
    {}
};

struct EngineToRendererBuffer : public EngineModeVariant<
        EngineToRendererBufferCUDA, EngineToRendererBufferCPU> {
    using BaseT::BaseT;

    inline void * enginePointer() const
    {
        if (isCuda) {
            return cuda.cudaImported.getDevicePointer();
        } else {
            return cpu.staging.ptr;
        }
    }

    inline VkDeviceAddress devAddr(const DeviceState &dev)
    {
        VkBuffer buf =
            isCuda ? cuda.devBuffer.buf.buffer : cpu.devBuffer.buf.buffer;
        return getDevAddr(dev, buf);
    }

    inline void toRenderer(const DeviceState &dev, VkCommandBuffer cmd,
                           VkAccessFlagBits pipeline_access,
                           VkPipelineStageFlagBits pipeline_stage)
    {
        if (!isCuda) {
            cpu.staging.flush(dev);

            VkBufferCopy buffer_copy {
                .srcOffset = 0,
                .dstOffset = 0,
                .size = cpu.numBytes,
            };

            dev.dt.cmdCopyBuffer(cmd, cpu.staging.buffer,
                cpu.devBuffer.buf.buffer, 1, &buffer_copy);

            if (pipeline_access != VK_ACCESS_NONE) {
                VkMemoryBarrier barrier;
                barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
                barrier.pNext = nullptr;
                barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                barrier.dstAccessMask = pipeline_access;

                dev.dt.cmdPipelineBarrier(cmd,
                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                    pipeline_stage, 0, 1,
                    &barrier, 0, nullptr, 0, nullptr);
            }
        }
    }
};

}
