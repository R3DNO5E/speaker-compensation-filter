#include "fir.h"

extern const float FIR_COEFF_44100[];
extern const float FIR_COEFF_88200[];
extern const float FIR_COEFF_176400[];
extern const float FIR_COEFF_48000[];
extern const float FIR_COEFF_96000[];
extern const float FIR_COEFF_192000[];

const struct fir_filter FIR_FILTERS[] = {
    {44100, FIR_COEFF_44100, 4095},
    {88200, FIR_COEFF_88200, 8191},
    {176400, FIR_COEFF_176400, 16383},
    {48000, FIR_COEFF_48000, 4095},
    {96000, FIR_COEFF_96000, 8191},
    {192000, FIR_COEFF_192000, 16383}
};
