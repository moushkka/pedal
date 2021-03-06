#include "soundloop.hpp"
#include <sndfile.h>

namespace deepness
{
    SoundLoop::SoundLoop(std::string const& filename)
        : m_handle(nullptr)
    {
        SF_INFO info = {0};
        m_handle = sf_open(filename.c_str(), SFM_READ, &info);
        if(!m_handle)
            throw Exception(sf_strerror(nullptr));
        if(info.channels != 1)
            throw Exception("Non-mono file");
        if(!info.seekable)
            throw Exception("Non-seekable file");
    }

    SoundLoop::SoundLoop() noexcept
    : m_handle(nullptr)
    {}

    SoundLoop::SoundLoop(SoundLoop &&other) noexcept
    : m_handle(nullptr)
    {
        *this = std::move(other);
    }

    SoundLoop &SoundLoop::operator=(SoundLoop &&other) noexcept
    {
        if(m_handle)
            sf_close(m_handle);
        m_handle = other.m_handle;
        other.m_handle = nullptr;
        return *this;
    }

    SoundLoop::~SoundLoop() noexcept
    {
        if(m_handle)
            sf_close(m_handle);
    }

    void SoundLoop::read(float *buffer, unsigned int samples)
    {
        while(samples)
        {
            auto count = sf_read_float(m_handle, buffer, samples);
            buffer += count;
            samples -= count;
            if(samples > 0)
            {
                auto result = sf_seek(m_handle, 0, SEEK_SET);
                if(result < 0)
                    throw Exception("Error seeking in file");
            }
        }
    }
}
