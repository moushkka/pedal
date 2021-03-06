#include <iostream>
#include <portaudio.h>
#include "audioobject.hpp"
#include <cmath>
#include "effects.hpp"
#include "drone.hpp"
#include <iomanip>
#include <unistd.h>
#include "webserver.hpp"
#include <boost/program_options.hpp>
#include "soundloop.hpp"

using namespace deepness;
using namespace std;

float average(const float *in, unsigned long samples)
{
    auto average = 0.f;
    for(size_t i = 0; i < samples; ++i)
        average += in[i];
    return average / samples;
}

float volume(const float *in, unsigned long samples)
{
    auto avg = average(in, samples);
    auto volume = 0.f;
    for(size_t i = 0; i < samples; ++i)
        volume += pow(in[i] - avg, 2);
    return sqrt(volume / static_cast<float>(samples));
}

SoundTransform CalculateVolume(std::function<void (float volume)> savefunc)
{
    return [savefunc = std::move(savefunc)](const float *in, float *out, unsigned long samples)
    {
        std::copy_n(in, samples, out);
        savefunc(volume(out, samples));
    };
}

SoundTransform CalculateHiLow(std::function<void (float hi, float low)> savefunc)
{
    return [savefunc = std::move(savefunc)](const float *in, float *out, unsigned long samples) {
        std::copy_n(in, samples, out);
        float max = std::numeric_limits<float>::lowest();
        float min = std::numeric_limits<float>::max();
        for(auto i = 0ul; i < samples; ++i)
        {
            max = std::max(max, in[i]);
            min = std::min(min, in[i]);
        }
        savefunc(max, min);
    };
}

SoundTransform printAverageVolume(std::function<void (const float *in, float *out, unsigned long samples)> func)
{
    return [func](const float *in, float *out, unsigned long samples)
    {
        func(in, out, samples);
        cout << setprecision(4) << fixed  << "average: " << setw(8) << average(out, samples) << " volume: " << setw(8) << volume(out, samples) << "\r";
    };
}

SoundTransform SaveBuffer(std::function<void (const float *buffer, unsigned long samples)> savefunc)
{
    return [savefunc = std::move(savefunc)](const float*in, float *out, unsigned long samples) {
        savefunc(in, samples);
        std::copy_n(in, samples, out);
    };
}

int main(int argc, char *argv[])
{
    namespace po = boost::program_options;
    po::options_description desc("Options");
    desc.add_options()
        ("help", "help")
        ("override-input", po::value<std::string>(), "A wavefile to use instead of microphone input");
    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);
    if(vm.count("help"))
    {
        std::cout << desc << std::endl;
        return 1;
    }
    std::vector<std::function<void (const float *, float *, unsigned long)>> transforms;
    if(vm.count("override-input"))
        transforms.push_back(SoundLoopTransform{SoundLoop{vm["override-input"].as<std::string>()}});
    double sampleRate = 44100;
    //auto effect = &passthrough;
    //auto effect = &fuzz;
    //auto effect = Delay(sampleRate);
    //auto effect = combine(Delay(sampleRate), &fuzz, &passthrough);
    //auto drone = Drone{sampleRate};
    //auto effect = combine(drone, Compress(5.f), &clip);
    //transforms.push_back(WetDryMix(OctaveDown(), Mixer(0.5f)));
    //transforms.push_back(WetDryMix(OctaveUp(), Mixer(0.5f)));
    //transforms.push_back(WetDryMix(chain({AbsOctaveUp(), HiPass(sampleRate, 1000.f), AbsOctaveUp(), HiPass(sampleRate, 1000.f)}), Mixer(.5f)));
    transforms.push_back(WetDryMix(chain({
                    HiPass(sampleRate, 10.f)
                        , LoPass(sampleRate, 100.f)
                        , SquareOctaveDown(1)
                        , HiPass(sampleRate, 1000.f)
                        }), Mixer(0.1f)));
    auto effect = combine(Compress(1.5f), &clip);
    //transforms.push_back(iterate(effect));
    //transforms.push_back(WetDryMix(chain({iterate(Drone{sampleRate}), HiPass(sampleRate, 1000.f)}), Mixer(1.f)));
    transforms.push_back(iterate(&clip));
    std::atomic<float> volume(0.f);
    transforms.push_back(CalculateVolume([&volume](float arg) {
                volume = arg;
            }));
    std::atomic<float> hi;
    std::atomic<float> low;
    transforms.push_back(CalculateHiLow([&hi, &low](float max, float min) {
                hi = max;
                low = min;
            }));
    std::mutex sendBufferListLock;
    std::deque<std::function<void (const float* in, unsigned long samples)>> sendBufferList;
    transforms.push_back(SaveBuffer([&sendBufferListLock, &sendBufferList, bufferBuffer = std::vector<float>()](const float * in, unsigned long samples) mutable {
                // look for zero crossings to start buffers
                if(!bufferBuffer.empty())
                {
                    std::copy_n(in, samples - bufferBuffer.size(), std::back_inserter(bufferBuffer));
                    while(true)
                    {
                        std::function<void (const float *in, unsigned long samples)> func;
                        {
                            std::lock_guard<std::mutex> lock(sendBufferListLock);
                            if(sendBufferList.empty())
                            {
                                bufferBuffer.clear();
                                return;
                            }
                            func = std::move(sendBufferList.front());
                            sendBufferList.pop_front();
                        }
                        func(bufferBuffer.data(), bufferBuffer.size());
                    }
                }
                else
                {
                    {
                        std::lock_guard<std::mutex> lock(sendBufferListLock);
                        if(sendBufferList.empty())
                            return;
                    }
                    for(auto i = 1ul; i < samples; ++i)
                    {
                        //if(in[i - 1] > 0.f != in[i] > 0.f)
                        if(in[i - 1] < 0.f && in[i] > 0.f)
                        {
                            std::copy_n(in + i, samples - i, std::back_inserter(bufferBuffer));
                            break;
                        }
                    }
                }
            }));
    AudioObject audio(chain(std::move(transforms)), sampleRate);
    Webserver server("http_root");
    using namespace json11;
    server.handleMessage("getoutvolume", [&volume](Json const& args, Webserver::SendFunc send) {
            Json message = Json::object {
                {"cmd", "outvolume"},
                {"args", volume.load()},
            };
            send(message.dump());
        });
    server.handleMessage("getouthilow", [&hi, &low](Json const& args, Webserver::SendFunc send) {
            Json message = Json::object {
                {"cmd", "outhilow"},
                {"args", Json(Json::array { hi.load(), low.load() })},
            };
            send(message.dump());
        });
    server.handleMessage("getdynamicparameters", [](Json const& args, Webserver::SendFunc send) {
            auto &id = args["id"];
            auto outargs = Json::object {
                {"params", Json::array { "testvar", "wetdrymix" }}
            };
            if(!id.is_null())
                outargs.insert(std::make_pair("id", id));
            auto message = Json{Json::object {
                    {"cmd", "dynamicparameters"},
                    {"args", Json(outargs)},
                }};
            send(message.dump());
        });
    server.handleMessage("getlatestbuffer", [&sendBufferListLock, &sendBufferList](Json const& args, Webserver::SendFunc send) {
            std::lock_guard<std::mutex> lock(sendBufferListLock);
            if(sendBufferList.size() > 100)
            {
                std::cerr << "sendBufferList full" << std::endl;
                return;
            }
            sendBufferList.push_back([id = args["id"], send = std::move(send)](const float *in, unsigned long samples) {
                    std::vector<float> buffer;
                    buffer.reserve(samples);
                    std::copy_n(in, samples, std::back_inserter(buffer));
                    auto outargs = Json::object {
                        {"buffer", buffer},
                    };
                    if(!id.is_null())
                        outargs.insert(std::make_pair("id", id));
                    auto message = Json{Json::object {
                            {"cmd", "latestbuffer"},
                            {"args", Json(outargs)},
                        }};
                    send(message.dump());
                });
        });
    std::cerr << "Press any key to stop" << std::endl;
    std::cin.get();
    //while(true) sleep(1);
    return 0;
}
