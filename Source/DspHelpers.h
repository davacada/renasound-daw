#pragma once
#include <JuceHeader.h>

// Small DSP building blocks shared by the built-in effects.

namespace rs
{

using Range = juce::NormalisableRange<float>;

inline std::unique_ptr<juce::AudioParameterFloat> floatParam (const juce::String& id, const juce::String& name, Range r, float def, const char* unit = "")
{
    const juce::String u (unit);
    auto toText = [u] (float v, int)
    {
        auto trimmed = [] (float x) { auto t = juce::String (x, 2); if (t.contains (".")) t = t.trimCharactersAtEnd ("0").trimCharactersAtEnd ("."); return t; };
        const auto num = std::abs (v) >= 999.5f ? trimmed (std::abs (v) >= 9995.0f ? std::round (v / 100.0f) / 10.0f : v / 1000.0f) + "k"
                                                 : std::abs (v) < 9.95f ? juce::String (v, 1)
                                                                        : juce::String (juce::roundToInt (v));
        if (u == "%") return juce::String (juce::roundToInt (v)) + "%";
        return num + (u.startsWith (":") ? u : u.isNotEmpty() ? " " + u : juce::String());
    };
    return std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { id, 1 }, name, r, def,
                                                        juce::AudioParameterFloatAttributes().withLabel (unit)
                                                                                             .withStringFromValueFunction (toText));
}

inline Range skewed (float lo, float hi, float centre)
{
    Range r (lo, hi);
    r.setSkewForCentre (centre);
    return r;
}

// RBJ "Audio EQ Cookbook" biquad coefficients (normalised, a0 = 1). Computed
// without allocating, so they can be recalculated on the audio thread.
struct BiquadCoefs
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;

    enum class Type { Peak, LowShelf, HighShelf, LowPass, HighPass, BandPass };

    static BiquadCoefs make (Type t, double sr, double freq, double q, double gainDb)
    {
        freq = juce::jlimit (5.0, sr * 0.49, freq);
        q = juce::jmax (0.05, q);
        const double A = std::pow (10.0, gainDb / 40.0);
        const double w = juce::MathConstants<double>::twoPi * freq / sr;
        const double c = std::cos (w), s = std::sin (w);
        const double alpha = s / (2.0 * q);
        double b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;

        switch (t)
        {
            case Type::Peak:
                b0 = 1 + alpha * A; b1 = -2 * c; b2 = 1 - alpha * A;
                a0 = 1 + alpha / A; a1 = -2 * c; a2 = 1 - alpha / A;
                break;
            case Type::LowShelf:
            {
                const double sA = 2 * std::sqrt (A) * alpha;
                b0 = A * ((A + 1) - (A - 1) * c + sA); b1 = 2 * A * ((A - 1) - (A + 1) * c); b2 = A * ((A + 1) - (A - 1) * c - sA);
                a0 = (A + 1) + (A - 1) * c + sA;        a1 = -2 * ((A - 1) + (A + 1) * c);    a2 = (A + 1) + (A - 1) * c - sA;
                break;
            }
            case Type::HighShelf:
            {
                const double sA = 2 * std::sqrt (A) * alpha;
                b0 = A * ((A + 1) + (A - 1) * c + sA); b1 = -2 * A * ((A - 1) + (A + 1) * c); b2 = A * ((A + 1) + (A - 1) * c - sA);
                a0 = (A + 1) - (A - 1) * c + sA;        a1 = 2 * ((A - 1) - (A + 1) * c);      a2 = (A + 1) - (A - 1) * c - sA;
                break;
            }
            case Type::LowPass:
                b0 = (1 - c) / 2; b1 = 1 - c; b2 = (1 - c) / 2; a0 = 1 + alpha; a1 = -2 * c; a2 = 1 - alpha;
                break;
            case Type::HighPass:
                b0 = (1 + c) / 2; b1 = -(1 + c); b2 = (1 + c) / 2; a0 = 1 + alpha; a1 = -2 * c; a2 = 1 - alpha;
                break;
            case Type::BandPass:   // constant 0 dB peak
                b0 = alpha; b1 = 0; b2 = -alpha; a0 = 1 + alpha; a1 = -2 * c; a2 = 1 - alpha;
                break;
        }
        return { b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0 };
    }

    double magnitude (double freq, double sr) const
    {
        const auto w = juce::MathConstants<double>::twoPi * freq / sr;
        const std::complex<double> z1 = std::polar (1.0, -w), z2 = z1 * z1;
        return std::abs ((b0 + b1 * z1 + b2 * z2) / (1.0 + a1 * z1 + a2 * z2));
    }
};

// Transposed direct form II, up to two channels sharing one set of coefficients
struct Biquad
{
    BiquadCoefs c;
    double s1[2] {}, s2[2] {};

    void reset() { s1[0] = s1[1] = s2[0] = s2[1] = 0; }

    inline float process (float xf, int ch) noexcept
    {
        const double x = xf;
        const double y = c.b0 * x + s1[ch];
        s1[ch] = c.b1 * x - c.a1 * y + s2[ch];
        s2[ch] = c.b2 * x - c.a2 * y;
        return (float) y;
    }
};

} // namespace rs
