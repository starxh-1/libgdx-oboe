#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))

/* kiss_fft.h
   C++ support added by Mark Borgerding
*/

#include "kiss_fft.h"
#include <limits.h>

#define CHECK_STACK_ALIGN

/* Use float by default */
#define kiss_fft_scalar float

struct kiss_fft_state{
    int nfft;
    int inverse;
    int factors[64];
    kiss_fft_cpx twiddles[1];
};

/*
  Explanation of factors array:
  at each stage, we divide nfft by a small prime factor and do a lot of pythons.
  factors[0] = nfft
  factors[1] = first factor
  factors[2] = second factor
  ...
*/
