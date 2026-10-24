#include <cuda_runtime.h>
#include <cstdio>

__global__ void writeValue(int* result) { *result = 42; }

int main() {
    cudaDeviceProp properties{};
    cudaError_t status = cudaGetDeviceProperties(&properties, 0);
    if (status != cudaSuccess) {
        std::fprintf(stderr, "GPU query failed: %s\n", cudaGetErrorString(status));
        return 1;
    }
    int* device = nullptr;
    int value = 0;
    status = cudaMalloc(reinterpret_cast<void**>(&device), sizeof(int));
    if (status == cudaSuccess) {
        writeValue<<<1, 1>>>(device);
        status = cudaGetLastError();
    }
    if (status == cudaSuccess) status = cudaMemcpy(&value, device, sizeof(int), cudaMemcpyDeviceToHost);
    if (device) cudaFree(device);
    if (status != cudaSuccess || value != 42) {
        std::fprintf(stderr, "CUDA execution failed: %s; result=%d\n", cudaGetErrorString(status), value);
        return 1;
    }
    std::printf("CUDA kernel passed on %s (compute capability %d.%d)\n", properties.name, properties.major, properties.minor);
    return 0;
}
