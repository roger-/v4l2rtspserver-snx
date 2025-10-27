#pragma once

#include <memory>
#include <vector>
#include <string>
#include <cstring>
#include <linux/videodev2.h>

#include "DeviceInterface.h"
#include "snx/SnxCodecController.h"

// Adapter that exposes SnxCodecController as a DeviceInterface for V4L2DeviceSource
class SnxDeviceInterface : public DeviceInterface
{
public:
    SnxDeviceInterface(std::shared_ptr<SnxCodecController> controller,
                       SnxCodecController::StreamKind stream,
                       int width,
                       int height,
                       size_t bufferSize = (2 * 1024 * 1024))
        : m_controller(std::move(controller)),
          m_stream(stream),
          m_width(width),
          m_height(height),
          m_bufferSize(bufferSize)
    {
    }

    virtual ~SnxDeviceInterface() {}

    // Read a complete access unit from the controller into the provided buffer.
    virtual size_t read(char *buffer, size_t bufferSize)
    {
        if (!m_controller || !m_controller->isRunning())
            return 0;
        std::vector<unsigned char> data;
        timeval pts; pts.tv_sec = 0; pts.tv_usec = 0;
        bool key = false;
        if (!m_controller->readFrame(m_stream, data, pts, key))
        {
            // no frame currently available
            return 0;
        }

        // Parse Annex-B NALs to cache SPS/PPS
        cacheParameterSetsIfAny(data);
        // If keyframe but missing SPS/PPS prefix, prepend cached sets
        if (key && !startsWithSpsPps(data))
        {
            if (!m_sps.empty() && !m_pps.empty())
            {
                std::vector<unsigned char> fused;
                static const unsigned char sc4[4] = {0x00,0x00,0x00,0x01};
                // SPS
                fused.insert(fused.end(), sc4, sc4 + 4);
                fused.insert(fused.end(), m_sps.begin(), m_sps.end());
                // PPS
                fused.insert(fused.end(), sc4, sc4 + 4);
                fused.insert(fused.end(), m_pps.begin(), m_pps.end());
                // Original AU
                fused.insert(fused.end(), data.begin(), data.end());
                LOG(DEBUG) << "SNX: injected cached SPS(" << m_sps.size() << ")/PPS(" << m_pps.size() << ") before IDR (total " << fused.size() << ")";
                data.swap(fused);
            }
        }

        size_t n = data.size();
        if (n > bufferSize) n = bufferSize;
        if (n > 0) std::memcpy(buffer, data.data(), n);

        // Debug: log first few read sizes
        static int s_readCount = 0;
        if (s_readCount < 10)
        {
            LOG(DEBUG) << "SNX read(" << (m_stream==SnxCodecController::High?"high":"low")
                       << ") size=" << data.size() << " copied=" << n << (key?" key":"");
            s_readCount++;
        }
        return n;
    }

    // Return -1 so V4L2DeviceSource uses its internal capture thread rather than
    // Live555 background read handling on a non-POSIX-readable fd.
    virtual int getFd() { return -1; }

    virtual unsigned long getBufferSize()
    {
        return (unsigned long)m_bufferSize;
    }

    virtual bool requestKeyFrame()
    {
        if (!m_controller) return false;
        return m_controller->requestIDR(m_stream);
    }

    virtual int getWidth() { return m_width; }
    virtual int getHeight() { return m_height; }
    virtual int getVideoFormat() { return V4L2_PIX_FMT_H264; }

private:
    // Very small Annex-B parser helpers
    static inline int nalUnitType(unsigned char b) { return b & 0x1F; }
    static bool isStartCode3(const unsigned char *p) { return p[0]==0x00 && p[1]==0x00 && p[2]==0x01; }
    static bool isStartCode4(const unsigned char *p) { return p[0]==0x00 && p[1]==0x00 && p[2]==0x00 && p[3]==0x01; }
    static size_t findStartCode(const std::vector<unsigned char> &buf, size_t off)
    {
        for (size_t i = off; i + 3 < buf.size(); ++i)
        {
            if (isStartCode4(&buf[i])) return i + 4;
            if (i + 2 < buf.size() && isStartCode3(&buf[i])) return i + 3;
        }
        return std::string::npos;
    }

    void cacheParameterSetsIfAny(const std::vector<unsigned char> &buf)
    {
        // Iterate over NAL units and cache SPS/PPS payloads without start codes
        size_t pos = findStartCode(buf, 0);
        while (pos != std::string::npos && pos < buf.size())
        {
            // next start
            size_t next = findStartCode(buf, pos);
            size_t nalStart = pos;
            size_t nalEnd = buf.size();
            if (next != std::string::npos)
            {
                // 'next' points AFTER a start code; explicitly find where it began
                // This avoids off-by-one errors with mixed 3/4-byte start codes
                if (next >= 4 && isStartCode4(&buf[next-4])) {
                    nalEnd = next - 4;
                } else if (next >= 3 && isStartCode3(&buf[next-3])) {
                    nalEnd = next - 3;
                } else {
                    // Shouldn't happen if findStartCode is correct
                    nalEnd = next;
                }
            }
            if (nalEnd <= nalStart) break;
            unsigned char header = buf[nalStart];
            int type = nalUnitType(header);
            if (type == 7)
            {
                m_sps.assign(buf.begin() + nalStart, buf.begin() + nalEnd);
            }
            else if (type == 8)
            {
                m_pps.assign(buf.begin() + nalStart, buf.begin() + nalEnd);
            }
            pos = next;
        }
    }

    bool startsWithSpsPps(const std::vector<unsigned char> &buf) const
    {
        // Check if AU begins with SPS then PPS before slices
        size_t pos = findStartCode(buf, 0);
        if (pos == std::string::npos || pos >= buf.size()) return false;
        int t1 = nalUnitType(buf[pos]);
        if (t1 != 7) return false;
        size_t next = findStartCode(buf, pos);
        if (next == std::string::npos || next >= buf.size()) return false;
        int t2 = nalUnitType(buf[next]);
        return t2 == 8;
    }

    std::shared_ptr<SnxCodecController> m_controller;
    SnxCodecController::StreamKind m_stream;
    int m_width;
    int m_height;
    size_t m_bufferSize;
    std::vector<unsigned char> m_sps;
    std::vector<unsigned char> m_pps;
};
