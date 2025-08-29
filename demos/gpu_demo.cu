#include <cuda_runtime.h>
__global__ void noop() {}
int main() { noop<<<1,1>>>(); cudaDeviceSynchronize(); return 0; }
