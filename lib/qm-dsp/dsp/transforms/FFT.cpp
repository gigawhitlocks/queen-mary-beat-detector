#include "dsp/transforms/FFT.h"
#include "maths/MathUtilities.h"
#include <cmath>
#include <stdexcept>

/* ===== FFT (complex-to-complex) ===== */

class FFT::D {
public:
    D(int n) : m_n(n) {
        m_planf = kiss_fft_alloc(m_n, 0, NULL, NULL);
        m_plani = kiss_fft_alloc(m_n, 1, NULL, NULL);
        m_kin = new kiss_fft_cpx[m_n];
        m_kout = new kiss_fft_cpx[m_n];
    }
    ~D() {
        kiss_fft_free(m_planf);
        kiss_fft_free(m_plani);
        delete[] m_kin;
        delete[] m_kout;
    }
    void process(bool inverse, const double *ri, const double *ii,
                 double *ro, double *io) {
        for (int i = 0; i < m_n; ++i) {
            m_kin[i].r = (float)ri[i];
            m_kin[i].i = (float)(ii ? ii[i] : 0.0);
        }
        if (!inverse) {
            kiss_fft(m_planf, m_kin, m_kout);
            for (int i = 0; i < m_n; ++i) {
                ro[i] = m_kout[i].r;
                io[i] = m_kout[i].i;
            }
        } else {
            kiss_fft(m_plani, m_kin, m_kout);
            double scale = 1.0 / m_n;
            for (int i = 0; i < m_n; ++i) {
                ro[i] = m_kout[i].r * scale;
                io[i] = m_kout[i].i * scale;
            }
        }
    }
    int m_n;
    kiss_fft_cfg m_planf;
    kiss_fft_cfg m_plani;
    kiss_fft_cpx *m_kin;
    kiss_fft_cpx *m_kout;
};

FFT::FFT(int n) : m_d(new D(n)) {}
FFT::~FFT() { delete m_d; }

void FFT::process(bool inverse, const double *ri, const double *ii,
                  double *ro, double *io) {
    m_d->process(inverse, ri, ii, ro, io);
}

/* ===== FFTReal (real-to-complex) ===== */

class FFTReal::D {
public:
    D(int n) : m_n(n) {
        if (n % 2) throw std::invalid_argument("nsamples must be even in FFTReal");
        m_planf = kiss_fftr_alloc(m_n, 0, nullptr, nullptr);
        m_plani = kiss_fftr_alloc(m_n, 1, nullptr, nullptr);
        m_c = new kiss_fft_cpx[m_n];
    }
    ~D() {
        kiss_fftr_free(m_planf);
        kiss_fftr_free(m_plani);
        delete[] m_c;
    }
    void forward(const double *ri, double *ro, double *io) {
        // Convert double[] → float[] (kissfft expects float input)
        std::vector<float> finput(m_n);
        for (int i = 0; i < m_n; ++i) finput[i] = static_cast<float>(ri[i]);
        kiss_fftr(m_planf, finput.data(), m_c);
        for (int i = 0; i <= m_n/2; ++i) {
            ro[i] = static_cast<double>(m_c[i].r);
            io[i] = static_cast<double>(m_c[i].i);
        }
        // Fill the redundant conjugate half (kiss_fftr only returns n/2+1 bins)
        for (int i = 1; i < m_n/2; ++i) {
            ro[m_n - i] = ro[i];
            io[m_n - i] = -io[i];
        }
    }
    void forwardMagnitude(const double *ri, double *mo) {
        double *io = new double[m_n];
        forward(ri, mo, io);
        for (int i = 0; i < m_n; ++i)
            mo[i] = sqrt(mo[i]*mo[i] + io[i]*io[i]);
        delete[] io;
    }
    void inverse(const double *ri, const double *ii, double *ro) {
        for (int i = 0; i < m_n/2 + 1; ++i) {
            m_c[i].r = static_cast<float>(ri[i]);
            m_c[i].i = static_cast<float>(ii[i]);
        }
        // kiss_fftri writes float[] output; convert to double[]
        std::vector<float> foutput(m_n);
        kiss_fftri(m_plani, m_c, foutput.data());
        double scale = 1.0 / m_n;
        for (int i = 0; i < m_n; ++i) ro[i] = static_cast<double>(foutput[i]) * scale;
    }
    int m_n;
    kiss_fftr_cfg m_planf;
    kiss_fftr_cfg m_plani;
    kiss_fft_cpx *m_c;
};

FFTReal::FFTReal(int n) : m_d(new D(n)) {}
FFTReal::~FFTReal() { delete m_d; }
void FFTReal::forward(const double *ri, double *ro, double *io) { m_d->forward(ri, ro, io); }
void FFTReal::forwardMagnitude(const double *ri, double *mo) { m_d->forwardMagnitude(ri, mo); }
void FFTReal::inverse(const double *ri, const double *ii, double *ro) { m_d->inverse(ri, ii, ro); }
