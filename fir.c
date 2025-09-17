#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <immintrin.h>
#include <assert.h>

#include "fir.h"

#define SIMD_WIDTH_AVX512 16
#define OUTPUT_UNROLL_FACTOR 4

static void fir_filter_apply_scalar(int len, const float *coeff, int count,
                                    const float *samples, float *output) {
    for (int i = 0; i < count; i++) {
        float sum = 0.0f;
        for (int j = 0; j < len; j++) {
            sum += coeff[j] * samples[i + j];
        }
        output[i] = sum;
    }
}

static void fir_filter_apply_avx512(int len, const float *coeff, int count,
                                    const float *samples, float *output) {
    const int vectorized_len = (len / SIMD_WIDTH_AVX512) * SIMD_WIDTH_AVX512;
    const int vectorized_count = (count / OUTPUT_UNROLL_FACTOR) * OUTPUT_UNROLL_FACTOR;

    for (int i = 0; i < vectorized_count; i += OUTPUT_UNROLL_FACTOR) {
        __m512 sum_vec0 = _mm512_setzero_ps();
        __m512 sum_vec1 = _mm512_setzero_ps();
        __m512 sum_vec2 = _mm512_setzero_ps();
        __m512 sum_vec3 = _mm512_setzero_ps();

        for (int j = 0; j < vectorized_len; j += SIMD_WIDTH_AVX512) {
            __m512 coeff_vec = _mm512_loadu_ps(&coeff[j]);

            __m512 samples_vec0 = _mm512_loadu_ps(&samples[i + 0 + j]);
            __m512 samples_vec1 = _mm512_loadu_ps(&samples[i + 1 + j]);
            __m512 samples_vec2 = _mm512_loadu_ps(&samples[i + 2 + j]);
            __m512 samples_vec3 = _mm512_loadu_ps(&samples[i + 3 + j]);

            sum_vec0 = _mm512_fmadd_ps(coeff_vec, samples_vec0, sum_vec0);
            sum_vec1 = _mm512_fmadd_ps(coeff_vec, samples_vec1, sum_vec1);
            sum_vec2 = _mm512_fmadd_ps(coeff_vec, samples_vec2, sum_vec2);
            sum_vec3 = _mm512_fmadd_ps(coeff_vec, samples_vec3, sum_vec3);
        }

        float sum0 = _mm512_reduce_add_ps(sum_vec0);
        float sum1 = _mm512_reduce_add_ps(sum_vec1);
        float sum2 = _mm512_reduce_add_ps(sum_vec2);
        float sum3 = _mm512_reduce_add_ps(sum_vec3);

        for (int j = vectorized_len; j < len; j++) {
            float c = coeff[j];
            sum0 += c * samples[i + 0 + j];
            sum1 += c * samples[i + 1 + j];
            sum2 += c * samples[i + 2 + j];
            sum3 += c * samples[i + 3 + j];
        }

        output[i + 0] = sum0;
        output[i + 1] = sum1;
        output[i + 2] = sum2;
        output[i + 3] = sum3;
    }

    for (int i = vectorized_count; i < count; i++) {
        __m512 sum_vec = _mm512_setzero_ps();

        for (int j = 0; j < vectorized_len; j += SIMD_WIDTH_AVX512) {
            __m512 coeff_vec = _mm512_loadu_ps(&coeff[j]);
            __m512 samples_vec = _mm512_loadu_ps(&samples[i + j]);
            sum_vec = _mm512_fmadd_ps(coeff_vec, samples_vec, sum_vec);
        }

        float sum = _mm512_reduce_add_ps(sum_vec);

        for (int j = vectorized_len; j < len; j++) {
            sum += coeff[j] * samples[i + j];
        }

        output[i] = sum;
    }
}

void fir_filter_apply(const struct fir_filter *filter, const struct delay_line *delay_line,
                      int count, float *output) {
    if (!filter || !delay_line || !output || count <= 0) {
        return;
    }

    const float *samples = delay_line->buffer + delay_line->index - filter->order - count;

#ifdef __AVX512F__
    fir_filter_apply_avx512(filter->order, filter->coeffs, count, samples, output);
#else
    fir_filter_apply_scalar(filter->order, filter->coeffs, count, samples, output);
#endif
}

void delay_line_append_samples(const struct fir_filter *filter, struct delay_line *delay_line,
                               const float *samples, int count) {
    if (!filter || !delay_line || !samples || count <= 0) {
        return;
    }

    float *buffer_write = delay_line->buffer + delay_line->index;
    float *buffer_mirror = buffer_write - (filter->order * 2);
    const float *buffer_end = delay_line->buffer + delay_line->size;

    for (int i = 0; i < count; i++) {
        *buffer_write = samples[i];
        *buffer_mirror = samples[i];

        buffer_write++;
        buffer_mirror++;

        if (buffer_write == buffer_end) {
            buffer_write = buffer_mirror;
            buffer_mirror = delay_line->buffer;
        }
    }

    delay_line->index = buffer_write - delay_line->buffer;
}

void fir_filter_free(const struct fir_filter *filter) {
    if (filter) {
        free((void *) filter->coeffs);
        free((void *) filter);
    }
}

struct fir_filter *fir_filter_clone(const struct fir_filter *filter) {
    if (!filter) {
        return NULL;
    }

    struct fir_filter *new_filter = malloc(sizeof(struct fir_filter));
    if (!new_filter) {
        fprintf(stderr, "Failed to allocate memory for FIR filter\n");
        return NULL;
    }

    float *coeffs_copy = malloc(sizeof(float) * filter->order);
    if (!coeffs_copy) {
        fprintf(stderr, "Failed to allocate memory for FIR filter coefficients\n");
        free(new_filter);
        return NULL;
    }

    memcpy(coeffs_copy, filter->coeffs, sizeof(float) * filter->order);

    new_filter->order = filter->order;
    new_filter->rate = filter->rate;
    new_filter->coeffs = coeffs_copy;

    return new_filter;
}

void delay_line_free(struct delay_line *delay_line) {
    if (delay_line) {
        free(delay_line->buffer);
        free(delay_line);
    }
}

struct delay_line *delay_line_init(size_t size) {
    if (size == 0) {
        fprintf(stderr, "Invalid delay line size: %zu\n", size);
        return NULL;
    }

    struct delay_line *delay_line = malloc(sizeof(struct delay_line));
    if (!delay_line) {
        fprintf(stderr, "Failed to allocate memory for delay line\n");
        return NULL;
    }

    delay_line->buffer = calloc(size, sizeof(float));
    if (!delay_line->buffer) {
        fprintf(stderr, "Failed to allocate memory for delay line buffer\n");
        free(delay_line);
        return NULL;
    }

    delay_line->size = size;
    delay_line->index = size - 1;

    return delay_line;
}
