#pragma once

#include <memory>
#include <string>
#include <vector>
#include <sys/time.h>
#include <cstddef>

class SnxCodecController : public std::enable_shared_from_this<SnxCodecController>
{
public:
    enum StreamKind { High, Low };

    struct StreamParams {
        unsigned width;
        unsigned height;
        unsigned fps;
        unsigned bitrate;
        unsigned gop;
        unsigned scale;
        StreamParams() : width(0), height(0), fps(0), bitrate(0), gop(0), scale(1) {}
    };

    struct DeviceConfig {
        std::string ispDevice;
        std::string m2mDevice;
        std::string capDevice;
        DeviceConfig() {}
    };

    SnxCodecController();
    ~SnxCodecController();

    SnxCodecController(const SnxCodecController &) = delete;
    SnxCodecController &operator=(const SnxCodecController &) = delete;

    bool start(const StreamParams &high, const StreamParams &low, const DeviceConfig &devices);
    void stop();

    bool isRunning() const;

    bool readFrame(StreamKind stream, std::vector<unsigned char> &buffer, timeval &presentation, bool &isKeyFrame);
    int getPollFd(StreamKind stream) const;
    std::size_t getMaxFrameSize(StreamKind stream) const;

    // Best-effort request for an IDR picture on the given stream. Returns true if the request was issued.
    bool requestIDR(StreamKind stream);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
