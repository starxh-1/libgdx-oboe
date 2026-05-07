#include "_kiss_fft_guts.h"

/* The basic idea is to optimize for small factors like 2, 3, 4, 5. */

static void kf_bfly2(kiss_fft_cpx * Fout, const size_t fstride, const kiss_fft_cfg st, int m) {
    kiss_fft_cpx * Fout2;
    kiss_fft_cpx * tw1 = st->twiddles;
    Fout2 = Fout + m;
    do {
        kiss_fft_cpx t;
        t.r = Fout2->r * tw1->r - Fout2->i * tw1->i;
        t.i = Fout2->r * tw1->i + Fout2->i * tw1->r;
        tw1 += fstride;
        Fout2->r = Fout->r - t.r;
        Fout2->i = Fout->i - t.i;
        Fout->r += t.r;
        Fout->i += t.i;
        ++Fout2;
        ++Fout;
    } while (--m);
}

static void kf_work(kiss_fft_cpx * Fout, const kiss_fft_cpx * f, const size_t fstride, int in_stride, int * factors, const kiss_fft_cfg st) {
    kiss_fft_cpx * Fout_beg = Fout;
    int p = *factors++; /* the radix  */
    int m = *factors++; /* stage's nfft/p  */
    kiss_fft_cpx * Fout_end = Fout + p * m;

    if (m == 1) {
        do {
            *Fout = *f;
            f += fstride * in_stride;
        } while (++Fout != Fout_end);
    } else {
        do {
            kf_work(Fout, f, fstride * p, in_stride, factors, st);
            f += fstride * in_stride;
        } while ((Fout += m) != Fout_end);
    }

    Fout = Fout_beg;
    switch (p) {
        case 2: kf_bfly2(Fout, fstride, st, m); break;
        default: /* Generic radix if needed, but we mostly use power of 2 */ break;
    }
}

kiss_fft_cfg kiss_fft_alloc(int nfft, int inverse_fft, void * mem, size_t * lenmem) {
    kiss_fft_cfg st = NULL;
    size_t memneeded = sizeof(struct kiss_fft_state) + sizeof(kiss_fft_cpx) * (nfft - 1);

    if (lenmem == NULL) {
        st = (kiss_fft_cfg) malloc(memneeded);
    } else {
        if (*lenmem >= memneeded) st = (kiss_fft_cfg) mem;
        *lenmem = memneeded;
    }
    if (st) {
        st->nfft = nfft;
        st->inverse = inverse_fft;
        for (int i = 0; i < nfft; ++i) {
            const double pi = 3.14159265358979323846264338327;
            double phase = -2 * pi * i / nfft;
            if (st->inverse) phase *= -1;
            st->twiddles[i].r = (float) cos(phase);
            st->twiddles[i].i = (float) sin(phase);
        }
        /* Factorize nfft (simplified: assume power of 2 for now) */
        int n = nfft;
        int * f = st->factors;
        while (n > 1) {
            *f++ = 2;
            *f++ = n / 2;
            n /= 2;
        }
    }
    return st;
}

void kiss_fft(kiss_fft_cfg cfg, const kiss_fft_cpx *fin, kiss_fft_cpx *fout) {
    kf_work(fout, fin, 1, 1, cfg->factors, cfg);
}

void kiss_fft_cleanup(void) {}
