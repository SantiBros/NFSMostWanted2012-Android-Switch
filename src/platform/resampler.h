// Sample-rate conversion for the sound output (FMOD mixes at 24 kHz; the
// Switch's audio output runs at 48 kHz).
//
// A windowed-sinc (Kaiser) interpolator with 32 taps: the converted sound
// keeps its high frequencies without the "metallic" mirror images that
// straight-line interpolation adds. Rates are integers and the position is
// kept as an exact fraction, so the conversion never drifts.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace platform {

class Resampler {
public:
    static constexpr int kTaps = 32;     // 16 input samples on each side of the output position
    static constexpr int kPhases = 256;  // filter tables between two input samples
    static constexpr double kPi = 3.14159265358979323846;

    void configure(int in_rate, int out_rate) {
        in_rate_ = in_rate;
        out_rate_ = out_rate;
        frac_ = 0;
        // Pass band up to 94% of the lower Nyquist frequency.
        const double cutoff = 0.5 * 0.94 * std::min(1.0, (double)out_rate / in_rate);  // cycles per input sample
        const double beta = 8.5;
        const double i0b = bessel_i0(beta);
        table_.assign((size_t)(kPhases + 1) * kTaps, 0.f);
        for (int p = 0; p <= kPhases; p++) {
            const double f = (double)p / kPhases;
            double sum = 0;
            float* row = &table_[(size_t)p * kTaps];
            for (int j = 0; j < kTaps; j++) {
                const double t = j - (kTaps / 2 - 1) - f;  // input sample j's distance from the output position
                const double x = 2 * cutoff * t;
                const double sinc = std::abs(x) < 1e-9 ? 1.0 : std::sin(kPi * x) / (kPi * x);
                const double r = t / (kTaps / 2);
                const double w = std::abs(r) >= 1 ? 0.0 : bessel_i0(beta * std::sqrt(1 - r * r)) / i0b;
                row[j] = (float)(2 * cutoff * sinc * w);
                sum += row[j];
            }
            for (int j = 0; j < kTaps; j++) row[j] = (float)(row[j] / sum);  // unity gain at every position
        }
        // History: the first output lines up with the first input sample.
        hist_.assign((size_t)(kTaps / 2 - 1) * 2, 0.f);
        pos_ = kTaps / 2 - 1;
    }

    // Converts `frames` interleaved stereo frames and appends the result to out.
    void process(const int16_t* in, int frames, std::vector<int16_t>& out) {
        for (int i = 0; i < frames * 2; i++) hist_.push_back((float)in[i]);
        const int avail = (int)(hist_.size() / 2);
        while (pos_ + kTaps / 2 < avail) {
            const int base = pos_ - (kTaps / 2 - 1);
            const double fp = (double)frac_ / out_rate_ * kPhases;
            const int p = (int)fp;
            const float a = (float)(fp - p);
            const float* r0 = &table_[(size_t)p * kTaps];
            const float* r1 = r0 + kTaps;
            const float* x = &hist_[(size_t)base * 2];
            float l = 0, r = 0;
            for (int j = 0; j < kTaps; j++) {
                const float c = r0[j] + (r1[j] - r0[j]) * a;
                l += x[j * 2] * c;
                r += x[j * 2 + 1] * c;
            }
            out.push_back(clamp16(l));
            out.push_back(clamp16(r));
            frac_ += in_rate_;
            while (frac_ >= (int64_t)out_rate_) {
                frac_ -= out_rate_;
                pos_++;
            }
        }
        // Drop the input that no later output needs.
        const int keep_from = pos_ - (kTaps / 2 - 1);
        if (keep_from > 0) {
            hist_.erase(hist_.begin(), hist_.begin() + (size_t)keep_from * 2);
            pos_ -= keep_from;
        }
    }

private:
    static double bessel_i0(double x) {
        double sum = 1, term = 1;
        for (int k = 1; k < 40; k++) {
            term *= (x / (2 * k)) * (x / (2 * k));
            sum += term;
            if (term < 1e-12 * sum) break;
        }
        return sum;
    }
    static int16_t clamp16(float v) { return (int16_t)std::lround(std::clamp(v, -32768.f, 32767.f)); }

    int in_rate_ = 1, out_rate_ = 1;
    int64_t frac_ = 0;  // output position between input samples pos_ and pos_+1, in 1/out_rate_ steps
    int pos_ = 0;       // input frame (in hist_) at or just before the next output
    std::vector<float> table_;
    std::vector<float> hist_;  // interleaved stereo input not yet used up
};

}  // namespace platform
