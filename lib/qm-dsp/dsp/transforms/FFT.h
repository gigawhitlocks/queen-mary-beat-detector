/* QM DSP: FFT wrappers for PhaseVocoder (wraps kissfft) */

#ifndef QM_DSP_FFT_H
#define QM_DSP_FFT_H

#include <cmath>
#include <stdexcept>
#include "kissfft/kiss_fft.h"
#include "kissfft/kiss_fftr.h"

class FFT  
{
public:
    FFT(int nsamples);
    ~FFT();
    void process(bool inverse,
                 const double *realIn, const double *imagIn,
                 double *realOut, double *imagOut);
private:
    class D;
    D *m_d;
};

class FFTReal
{
public:
    FFTReal(int nsamples);
    ~FFTReal();
    void forward(const double *realIn, double *realOut, double *imagOut);
    void forwardMagnitude(const double *realIn, double *magOut);
    void inverse(const double *realIn, const double *imagIn, double *realOut);
private:
    class D;
    D *m_d;
};

#endif
