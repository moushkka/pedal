#pragma once

#include <vector>

namespace deepness
{
    template <typename T>
    T sign(T val)
    {
        return (T(0) < val) - (val < T(0));
    }

    std::function<float (float)> combine(std::function<float (float)> a, std::function<float (float)> b)
    {
        return [a,b](float in) -> float
        {
            return b(a(in));
        };
    }

    float passthrough(float in, double)
    {
        return in;
    }

    float fuzz(float in)
    {
        return sign(in) * std::pow(std::abs(in), 0.7f);
    }

    class Delay
    {
    public:
        Delay(double sampleRate)
            :m_pos(0)
            ,m_samples(static_cast<size_t>(sampleRate * 0.1), 0.f)
            ,m_sampleRate(sampleRate)
        {}
        float operator()(float in)
        {
            m_pos = (m_pos + 1) % m_samples.size();
            auto inpos = (m_pos - 1) % m_samples.size();
            auto output = 0.9f * in + 0.5f * m_samples[m_pos];
            m_samples[inpos] = output;
            return output;
        }
    private:
        std::vector<float> m_samples;
        std::size_t m_pos;
        double m_sampleRate;
    };
}
