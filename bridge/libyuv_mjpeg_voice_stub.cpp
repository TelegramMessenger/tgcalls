#include <cstddef>
#include <cstdint>

namespace libyuv {
extern "C" {

int MJPGToARGB(const uint8_t *sample, size_t sample_size, uint8_t *dst_argb, int dst_stride_argb, int src_width,
               int src_height, int dst_width, int dst_height) {
    (void)sample;
    (void)sample_size;
    (void)dst_argb;
    (void)dst_stride_argb;
    (void)src_width;
    (void)src_height;
    (void)dst_width;
    (void)dst_height;
    return -1;
}

int MJPGToI420(const uint8_t *sample, size_t sample_size, uint8_t *dst_y, int dst_stride_y, uint8_t *dst_u,
               int dst_stride_u, uint8_t *dst_v, int dst_stride_v, int src_width, int src_height, int dst_width,
               int dst_height) {
    (void)sample;
    (void)sample_size;
    (void)dst_y;
    (void)dst_stride_y;
    (void)dst_u;
    (void)dst_stride_u;
    (void)dst_v;
    (void)dst_stride_v;
    (void)src_width;
    (void)src_height;
    (void)dst_width;
    (void)dst_height;
    return -1;
}

} // extern "C"
} // namespace libyuv
