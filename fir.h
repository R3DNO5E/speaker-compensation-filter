#ifndef FIR_H
#define FIR_H

#include <stddef.h>

struct fir_filter {
    int rate;
    const float *coeffs;
    int order;
};

struct delay_line {
    float *buffer;
    size_t size;
    size_t index;
};

extern const struct fir_filter FIR_FILTERS[];

void fir_filter_apply(const struct fir_filter *filter, const struct delay_line *delay_line, 
                     int count, float *output);

void delay_line_append_samples(const struct fir_filter *filter, struct delay_line *delay_line, 
                              const float *samples, int count);

void fir_filter_free(const struct fir_filter *filter);
struct fir_filter *fir_filter_clone(const struct fir_filter *filter);
struct delay_line *delay_line_init(size_t size);
void delay_line_free(struct delay_line *delay_line);

#endif
