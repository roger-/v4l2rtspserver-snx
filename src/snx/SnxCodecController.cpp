#include "snx/SnxCodecController.h"

#include <algorithm>
#include <stddef.h>
#include <cstddef>
#include <cerrno>
#include <cstring>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "logger.h"


#ifdef HAVE_SNX_SDK
#include <linux/videodev2.h>
#include <snx_vc/snx_vc_lib.h>
#include <snx_rc/snx_rc_lib.h>
#include <snx_isp/isp_lib_api.h>
#endif

namespace
{
static const unsigned kDefaultFrameSizeBytes = 512 * 1024;

#ifdef HAVE_SNX_SDK
#ifndef V4L2_PIX_FMT_SNX420
#define V4L2_PIX_FMT_SNX420 v4l2_fourcc('S', '4', '2', '0')
#endif

static const int kDefaultBufferCount = 2;

int computeSuggestedQp(const SnxCodecController::StreamParams &params)
{
    if (params.bitrate == 0 || params.fps == 0 || params.width == 0 || params.height == 0)
    {
        return 30;
    }

    const double pixelsPerFrame = static_cast<double>(params.width) * static_cast<double>(params.height);
    const double bitsPerFrame = static_cast<double>(params.bitrate) / std::max(1u, params.fps);
    const double bitsPerPixel = bitsPerFrame / std::max(1.0, pixelsPerFrame);

    if (bitsPerPixel >= 0.10)
    {
        return 24;
    }
    if (bitsPerPixel >= 0.05)
    {
        return 27;
    }
    if (bitsPerPixel >= 0.025)
    {
        return 30;
    }
    return 33;
}

size_t estimateFrameBudget(const SnxCodecController::StreamParams &params)
{
    if (params.fps == 0 || params.bitrate == 0)
    {
        return static_cast<size_t>(kDefaultFrameSizeBytes);
    }

    const size_t bytesPerFrame = static_cast<size_t>(params.bitrate / 8ULL / std::max(1u, params.fps));
    const size_t minBudget = static_cast<size_t>(kDefaultFrameSizeBytes);
    return std::max(minBudget, bytesPerFrame * 2);
}

bool copyDeviceName(char *dest, size_t destSize, const std::string &name)
{
    if (name.empty())
    {
        return false;
    }
    if (name.size() >= destSize)
    {
        return false;
    }
    std::memset(dest, 0, destSize);
    std::memcpy(dest, name.c_str(), name.size());
    return true;
}

#endif // HAVE_SNX_SDK
} // namespace

struct SnxCodecController::Impl
{
    bool running;
#ifdef HAVE_SNX_SDK
    struct Session
    {
        snx_m2m ctx;
        snx_rc rc;      // Rate control state for CBR
        bool isM2M;
        bool active;
        bool codecInitialized;
        bool codecStarted;
        bool ispInitialized;
        bool ispStarted;
        Session()
            : isM2M(false)
            , active(false)
            , codecInitialized(false)
            , codecStarted(false)
            , ispInitialized(false)
            , ispStarted(false)
        {
            std::memset(&ctx, 0, sizeof(ctx));
            std::memset(&rc, 0, sizeof(rc));
        }
    // single-threaded usage; no mutex to support older toolchains
    };

    StreamParams highParams;
    StreamParams lowParams;
    DeviceConfig deviceConfig;
    Session highSession;
    Session lowSession;
    // single-threaded usage; no mutex to support older toolchains

    bool configureSession(Session &session,
                          const StreamParams &params,
                          const std::string &codecDevice,
                          const std::string &ispDevice,
                          bool isM2M,
                          unsigned int scale);

    void cleanupSession(Session &session);
    void cleanupSessionLocked(Session &session);

#endif // HAVE_SNX_SDK
};

#ifdef HAVE_SNX_SDK
bool SnxCodecController::Impl::configureSession(Session &session,
                          const StreamParams &params,
                          const std::string &codecDevice,
                          const std::string &ispDevice,
                          bool isM2M,
                          unsigned int scale)
{
    std::memset(&session.ctx, 0, sizeof(session.ctx));
    session.isM2M = isM2M;
    session.active = false;
    session.codecInitialized = false;
    session.codecStarted = false;
    session.ispInitialized = false;
    session.ispStarted = false;

    if (codecDevice.empty())
    {
        LOG(ERROR) << "SNX codec device path is empty";
        return false;
    }

    if (!copyDeviceName(session.ctx.codec_dev, sizeof(session.ctx.codec_dev), codecDevice))
    {
        LOG(ERROR) << "SNX codec device path too long: " << codecDevice;
        return false;
    }

    if (isM2M)
    {
        if (ispDevice.empty())
        {
            LOG(ERROR) << "SNX ISP device path is empty";
            return false;
        }

        if (!copyDeviceName(session.ctx.isp_dev, sizeof(session.ctx.isp_dev), ispDevice))
        {
            LOG(ERROR) << "SNX ISP device path too long: " << ispDevice;
            return false;
        }
    }
    else if (!ispDevice.empty())
    {
        // Some SDK builds read isp_dev string even in CAP; set it but do not open/initialize ISP
        (void)copyDeviceName(session.ctx.isp_dev, sizeof(session.ctx.isp_dev), ispDevice);
    }

    session.ctx.m2m = isM2M ? 1U : 0U;
    // Use the selected scaler factor for both sessions; CAP will attach to the scaled plane
    // Middleware expects a non-zero scale for internal math; using hiScale avoids divide-by-zero
    session.ctx.scale = scale; // values 1,2,4
    // Memory types:
    // - M2M: configure OUTPUT (raw-in) and CAPTURE (encoded-out)
    // - CAP: do NOT touch OUTPUT at all; only configure CAPTURE side; keep out_mem valid
        session.ctx.cap_mem = V4L2_MEMORY_MMAP;          // CAPTURE via MMAP is generally reliable
        session.ctx.out_mem = V4L2_MEMORY_USERPTR;       // Use USERPTR for both paths (SDK examples)
    if (isM2M)
    {
        session.ctx.width = params.width;
        session.ctx.height = params.height;
    }
    else
    {
        // CAP attach-mode: some SDK drops perform pre-bind math with WxH.
        // Use the HIGH/source geometry here to avoid zero-geometry divide errors.
        if (highSession.ctx.width > 0 && highSession.ctx.height > 0)
        {
            session.ctx.width = highSession.ctx.width;
            session.ctx.height = highSession.ctx.height;
        }
        else
        {
            // Fallback: use provided params (expected to reflect the high WxH upstream)
            session.ctx.width = params.width;
            session.ctx.height = params.height;
        }
    }
    // SNX FPS configuration based on SDK examples (snx_m2m_capture_2stream.c):
    // - M2M: Sets isp_fps and codec_fps independently
    // - CAP: Sets isp_fps to M2M's ISP FPS, codec_fps to requested rate
    if (isM2M) {
        // M2M: ISP fps drives the sensor/ISP, codec_fps controls encoding rate
        session.ctx.isp_fps = static_cast<int>(params.fps ? params.fps : 30u);
        session.ctx.codec_fps = static_cast<int>(params.fps ? params.fps : static_cast<unsigned>(session.ctx.isp_fps));
    } else {
        // CAP: Inherits ISP FPS from M2M (SDK pattern), codec_fps is independent
        // SDK example: v2_m2m->isp_fps = 30; (same as M2M) v2_m2m->codec_fps = V2_FRAME_RATE;
        int m2m_isp_fps = (highSession.ctx.isp_fps > 0) ? highSession.ctx.isp_fps : 30;
        session.ctx.isp_fps = m2m_isp_fps;  // Match M2M's ISP FPS per SDK
        session.ctx.codec_fps = static_cast<int>(params.fps ? params.fps : 30u);
    }
    if (session.ctx.isp_fps < 1) session.ctx.isp_fps = 1;
    if (session.ctx.codec_fps < 1) session.ctx.codec_fps = 1;
    // SDK constraint: codec_fps should not exceed isp_fps
    if (session.ctx.codec_fps > session.ctx.isp_fps) {
        LOG(WARN) << "Clamping codec_fps " << session.ctx.codec_fps 
                  << " to isp_fps " << session.ctx.isp_fps 
                  << " per SDK constraint";
        session.ctx.codec_fps = session.ctx.isp_fps;
    }
    session.ctx.bit_rate = static_cast<int>(params.bitrate);
    session.ctx.qp = computeSuggestedQp(params);
    session.ctx.gop = static_cast<int>(params.gop);
    // Even in CAP/attach mode, SNX middleware expects a non-zero buffer count to avoid FPE
    // Do NOT set ISP mem/dev for CAP; just keep m2m_buffers non-zero to satisfy SDK expectations
    session.ctx.m2m_buffers = kDefaultBufferCount; // use default (2) for both paths
    session.ctx.codec_fmt = V4L2_PIX_FMT_H264;
    session.ctx.out_fmt = V4L2_PIX_FMT_SNX420; // ensure raw/output format is set for SDK pre-init math
    // ISP memory only applicable for M2M; CAP does not use ISP
    session.ctx.isp_mem = isM2M ? V4L2_MEMORY_MMAP : 0;
    if (isM2M)
    {
        session.ctx.isp_fmt = V4L2_PIX_FMT_SNX420;
    }
    session.ctx.cap_index = -1;
    session.ctx.codec_fd = -1;
    session.ctx.isp_fd = -1;
    session.ctx.ds_font_num = 128; // align with SDK default
    session.ctx.flags = 0;

    LOG(INFO) << "SNX cfg m2m=" << (isM2M?1:0)
              << " scale=" << session.ctx.scale
              << " " << (unsigned)session.ctx.width << "x" << (unsigned)session.ctx.height
              << " isp_fps=" << session.ctx.isp_fps
              << " codec_fps=" << session.ctx.codec_fps
              << " gop=" << session.ctx.gop
              << " buf=" << session.ctx.m2m_buffers
              << " mem{c,o,i}=" << session.ctx.cap_mem << "," << session.ctx.out_mem << "," << session.ctx.isp_mem;

    if (isM2M)
    {
        session.ctx.isp_fd = snx_open_device(session.ctx.isp_dev);
        if (session.ctx.isp_fd < 0)
        {
            LOG(ERROR) << "Failed to open SNX ISP device '" << ispDevice << "': " << strerror(errno);
            return false;
        }
        LOG(INFO) << "snx_isp_init()";
        if (snx_isp_init(&session.ctx) != 0)
        {
            LOG(ERROR) << "snx_isp_init failed (" << strerror(errno) << ")";
            // The SDK requires at least 2 buffers; retry with the default if not already
            if (session.ctx.m2m_buffers < kDefaultBufferCount)
            {
                session.ctx.m2m_buffers = kDefaultBufferCount;
                LOG(WARN) << "Retry snx_isp_init with m2m_buffers=" << session.ctx.m2m_buffers;
                if (snx_isp_init(&session.ctx) != 0)
                {
                    LOG(ERROR) << "snx_isp_init fallback failed (" << strerror(errno) << ")";
                    cleanupSessionLocked(session);
                    return false;
                }
            }
        }
        session.ispInitialized = true;
    }

    LOG(INFO) << "open codec device: '" << codecDevice << "'";
    session.ctx.codec_fd = snx_open_device(session.ctx.codec_dev);
    if (session.ctx.codec_fd < 0)
    {
        LOG(ERROR) << "Failed to open SNX codec device '" << codecDevice << "': " << strerror(errno);
        // For CAP path, some platforms use the same codec node as M2M. Retry with m2m device.
        if (!isM2M && !deviceConfig.m2mDevice.empty() && deviceConfig.m2mDevice != codecDevice)
        {
            LOG(WARN) << "Retry opening codec device with m2m device '" << deviceConfig.m2mDevice << "'";
            if (!copyDeviceName(session.ctx.codec_dev, sizeof(session.ctx.codec_dev), deviceConfig.m2mDevice))
            {
                LOG(ERROR) << "Fallback codec device path too long: " << deviceConfig.m2mDevice;
                cleanupSessionLocked(session);
                return false;
            }
            session.ctx.codec_fd = snx_open_device(session.ctx.codec_dev);
            if (session.ctx.codec_fd < 0)
            {
                LOG(ERROR) << "Fallback open failed for codec device '" << deviceConfig.m2mDevice << "': " << strerror(errno);
                cleanupSessionLocked(session);
                return false;
            }
        }
        else
        {
            cleanupSessionLocked(session);
            return false;
        }
    }

    LOG(INFO) << "snx_codec_init()";
    if (snx_codec_init(&session.ctx) != 0)
    {
        LOG(ERROR) << "snx_codec_init failed (" << strerror(errno) << ")";

        // CAP-specific fallback: try flipping codec out_mem once, then try height alignment
        if (!isM2M)
        {
            int err = errno;
            // Flip CAP codec out_mem between MMAP and USERPTR
            unsigned oldMem = session.ctx.out_mem;
            session.ctx.out_mem = (oldMem == V4L2_MEMORY_MMAP) ? V4L2_MEMORY_USERPTR : V4L2_MEMORY_MMAP;
            LOG(WARN) << "CAP snx_codec_init failed (" << strerror(err) << "), retry with out_mem="
                      << ((session.ctx.out_mem==V4L2_MEMORY_MMAP)?"MMAP":"USERPTR");
            if (snx_codec_init(&session.ctx) == 0)
            {
                goto codec_init_ok;
            }
            // restore previous out_mem for subsequent fallbacks
            session.ctx.out_mem = oldMem;

            // CAP-specific fallback A: some SDKs require CAPTURE height to be multiple of 16
            err = errno;
            unsigned h = session.ctx.height;
            unsigned h16 = (h + 15u) & ~15u;
            if ((err == EINVAL || err == ENOEXEC) && h16 != h)
            {
                LOG(WARN) << "CAP snx_codec_init failed at " << session.ctx.width << "x" << h
                          << ", retry with height aligned to 16 -> " << session.ctx.width << "x" << h16;
                session.ctx.height = h16;
                if (snx_codec_init(&session.ctx) == 0)
                {
                    goto codec_init_ok;
                }
                // keep original error flow if retry fails
                LOG(ERROR) << "snx_codec_init still failed after height alignment (" << strerror(errno) << ")";
            }
        }

        // Fallback #1: For M2M only, switch OUTPUT to USERPTR and retry
        if (isM2M && session.ctx.out_mem == V4L2_MEMORY_MMAP)
        {
            LOG(WARN) << "Retry snx_codec_init with out_mem=USERPTR";
            session.ctx.out_mem = V4L2_MEMORY_USERPTR;
            if (snx_codec_init(&session.ctx) == 0)
            {
                goto codec_init_ok;
            }
            LOG(ERROR) << "snx_codec_init fallback failed (" << strerror(errno) << ")";
        }

        // Fallback #2: For CAP path, some platforms use the same codec node as M2M
        if (!isM2M && !deviceConfig.m2mDevice.empty() &&
            std::string(session.ctx.codec_dev) != deviceConfig.m2mDevice)
        {
            LOG(WARN) << "Retry snx_codec_init for CAP path using m2m codec node '" << deviceConfig.m2mDevice << "'";
            // Close previous codec fd if any
            if (session.ctx.codec_fd >= 0)
            {
                close(session.ctx.codec_fd);
                session.ctx.codec_fd = -1;
            }
            if (!copyDeviceName(session.ctx.codec_dev, sizeof(session.ctx.codec_dev), deviceConfig.m2mDevice))
            {
                LOG(ERROR) << "Fallback codec device path too long: " << deviceConfig.m2mDevice;
                cleanupSessionLocked(session);
                return false;
            }
            session.ctx.codec_fd = snx_open_device(session.ctx.codec_dev);
            if (session.ctx.codec_fd < 0)
            {
                LOG(ERROR) << "Fallback open failed for codec device '" << deviceConfig.m2mDevice << "': " << strerror(errno);
                cleanupSessionLocked(session);
                return false;
            }
            if (snx_codec_init(&session.ctx) == 0)
            {
                goto codec_init_ok;
            }
            LOG(ERROR) << "snx_codec_init still failed with m2m codec node (" << strerror(errno) << ")";
        }

        // All fallbacks failed
        cleanupSessionLocked(session);
        return false;
    }
codec_init_ok:
    session.codecInitialized = true;

    // Initialize Rate Control for H.264 CBR
    if (session.ctx.codec_fmt == V4L2_PIX_FMT_H264 && session.ctx.bit_rate > 0)
    {
        // Initialize RC struct for CBR (per SDK documentation)
        session.rc.width = session.ctx.width / session.ctx.scale;
        session.rc.height = session.ctx.height / session.ctx.scale;
        session.rc.codec_fd = session.ctx.codec_fd;
        session.rc.Targetbitrate = session.ctx.bit_rate;
        session.rc.framerate = session.ctx.codec_fps;  // CRITICAL: Use codec_fps, not isp_fps
        session.rc.gop = session.ctx.gop;              // Use GOP setting from ctx

        // Seed initial QP from rate control (required for CBR loop)
        // NOTE: snx_codec_rc_init() loads defaults and OVERWRITES snx_rc_ext values
        LOG(INFO) << "RC: targetBitrate=" << session.rc.Targetbitrate 
                  << " fps=" << session.rc.framerate << " gop=" << session.rc.gop;
        session.ctx.qp = snx_codec_rc_init(&session.rc, SNX_RC_INIT);
        LOG(INFO) << "RC initialized with QP=" << session.ctx.qp;

        // Disable motion detection features AFTER rc_init (which loads defaults)
        // These features dynamically adjust FPS/bitrate, causing stuttering
        session.rc.snx_rc_ext.mdrc_en = 0;        // Disable motion detection rate control
        session.rc.snx_rc_ext.md_cnt_en = 0;      // Disable motion detection low bitrate mode
        session.rc.snx_rc_ext.rc_update = 0;      // Prevent dynamic RC parameter updates
        snx_rc_ext_set(&session.rc.snx_rc_ext);
        LOG(DEBUG) << "Motion detection disabled (mdrc=0, md_cnt=0)";
    }

    if (isM2M)
    {
        LOG(INFO) << "snx_isp_start()";
        if (snx_isp_start(&session.ctx) != 0)
        {
            LOG(ERROR) << "snx_isp_start failed (" << strerror(errno) << ")";
            cleanupSessionLocked(session);
            return false;
        }
        session.ispStarted = true;
        
        // Configure ISP power line frequency after ISP start (settings persist until ISP stop)
        if (deviceConfig.powerLineFreq > 0)
        {
            if (snx_isp_light_frequency_set(deviceConfig.powerLineFreq) == 0)
            {
                LOG(INFO) << "ISP anti-flicker set to " << deviceConfig.powerLineFreq << "Hz";
            }
            else
            {
                LOG(WARN) << "Failed to set ISP power line frequency to " << deviceConfig.powerLineFreq << "Hz";
            }
        }
        
        // Small settle delay to avoid immediate REQBUFS conflicts
        usleep(50000);
    }

    LOG(INFO) << "snx_codec_start()";
    if (snx_codec_start(&session.ctx) != 0)
    {
        LOG(ERROR) << "snx_codec_start failed (" << strerror(errno) << ")";
        // Fallback: try with default buffer count if smaller
        if (session.ctx.m2m_buffers < kDefaultBufferCount)
        {
            session.ctx.m2m_buffers = kDefaultBufferCount;
            LOG(WARN) << "Retry snx_codec_start with m2m_buffers=" << session.ctx.m2m_buffers;
            if (snx_codec_start(&session.ctx) != 0)
            {
                LOG(ERROR) << "snx_codec_start fallback failed (" << strerror(errno) << ")";
                cleanupSessionLocked(session);
                return false;
            }
        }
    }
    session.codecStarted = true;

    // For CAP attach mode, log the bound coded size reported by the driver
    if (!isM2M && session.ctx.codec_fd >= 0)
    {
        struct v4l2_format f;
        std::memset(&f, 0, sizeof(f));
        f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(session.ctx.codec_fd, VIDIOC_G_FMT, &f) == 0)
        {
            LOG(INFO) << "CAP bound coded size " << f.fmt.pix.width << "x" << f.fmt.pix.height;
        }
    }

    if (session.ctx.gop > 0 && snx_codec_set_gop(&session.ctx) != 0)
    {
        LOG(WARN) << "snx_codec_set_gop failed";
    }

    session.active = true;
    return true;
}

void SnxCodecController::Impl::cleanupSession(Session &session)
{
    cleanupSessionLocked(session);
}

void SnxCodecController::Impl::cleanupSessionLocked(Session &session)
{
    if (session.codecStarted)
    {
        if (snx_codec_stop(&session.ctx) != 0)
        {
            LOG(WARN) << "snx_codec_stop failed";
        }
        session.codecStarted = false;
    }

    if (session.codecInitialized)
    {
        if (snx_codec_uninit(&session.ctx) != 0)
        {
            LOG(WARN) << "snx_codec_uninit failed";
        }
        session.codecInitialized = false;
    }

    if (session.isM2M)
    {
        if (session.ispStarted)
        {
            if (snx_isp_stop(&session.ctx) != 0)
            {
                LOG(WARN) << "snx_isp_stop failed";
            }
            session.ispStarted = false;
        }

        if (session.ispInitialized)
        {
            if (snx_isp_uninit(&session.ctx) != 0)
            {
                LOG(WARN) << "snx_isp_uninit failed";
            }
            session.ispInitialized = false;
        }
    }

    if (session.ctx.codec_fd >= 0)
    {
        close(session.ctx.codec_fd);
        session.ctx.codec_fd = -1;
    }

    if (session.ctx.isp_fd >= 0)
    {
        close(session.ctx.isp_fd);
        session.ctx.isp_fd = -1;
    }

    session.ctx.cap_index = -1;
    session.ctx.cap_bytesused = 0;
    session.ctx.flags = 0;

    session.active = false;
}
#endif // HAVE_SNX_SDK

SnxCodecController::SnxCodecController()
    : m_impl(new Impl())
{
    m_impl->running = false;
}

SnxCodecController::~SnxCodecController()
{
    stop();
}

bool SnxCodecController::start(const StreamParams &high, const StreamParams &low, const DeviceConfig &devices)
{
#ifdef HAVE_SNX_SDK
    stop();

    m_impl->highParams = high;
    m_impl->lowParams = low;
    m_impl->deviceConfig = devices;

    // CRITICAL FIX FOR DUAL STREAM:
    // The SNX codec driver's scale parameter determines the OUTPUT resolution of that path:
    // - scale=1: output at input resolution (full size)
    // - scale=2: output at 1/2 input resolution  
    // - scale=4: output at 1/4 input resolution
    // The driver does NOT produce multiple resolutions from one path!
    //
    // For dual streams, we need:
    // - M2M (high) with scale=1: Produces FULL resolution (1280x720)
    // - CAP (low) with scale=2: Attaches to M2M and produces HALF resolution (640x360)
    //
    // The CAP path reads from the M2M encoder and applies its own scale factor.
    // Both M2M and CAP need to use the SAME base scale for the pipeline, then CAP
    // applies additional scaling on top.
    unsigned hiScale = 1; // M2M always uses scale=1 for full resolution in dual stream mode
    if (low.width && low.height && low.fps)
    {
        // Dual stream mode: M2M uses scale=1, CAP will use the requested low.scale
        hiScale = 1;
    }
    else if (high.scale == 2 || high.scale == 4)
    {
        // Single stream mode: allow M2M to use scaling
        hiScale = high.scale;
    }

    if (!m_impl->configureSession(m_impl->highSession, high, devices.m2mDevice, devices.ispDevice, true, hiScale))
    {
        LOG(WARN) << "Failed to start SNX high stream with m2m='" << devices.m2mDevice << "' isp='" << devices.ispDevice << "'";
        // Fallback: try swapped device mapping (some platforms expose ISP/Codec nodes inverted)
        DeviceConfig swapped = devices;
        std::swap(swapped.m2mDevice, swapped.ispDevice);
        LOG(WARN) << "Retry SNX high stream with swapped devices m2m='" << swapped.m2mDevice << "' isp='" << swapped.ispDevice << "'";
        if (!m_impl->configureSession(m_impl->highSession, high, swapped.m2mDevice, swapped.ispDevice, true, hiScale))
        {
            LOG(ERROR) << "Failed to start SNX high stream with both device mappings";
            return false;
        }
        // Update device config to the working one so low stream uses consistent mapping
        m_impl->deviceConfig = swapped;
    }

    // Optional low stream: skip if low params are not set
    if (low.width == 0 || low.height == 0 || low.fps == 0)
    {
        LOG(NOTICE) << "SNX low stream disabled (single-stream mode)";
    }
    else
    {
    // Give the driver a short window to stabilize M2M path before enabling CAP path (settle)
    usleep(300000);
        // CAP path attaches to M2M and applies its OWN scale factor to produce scaled output.
        // Always use the M2M device node for CAP path to ensure attach stability.
        std::string capNode = m_impl->deviceConfig.m2mDevice;
        // CAP uses its own scale factor (low.scale) to produce scaled output
        unsigned lowScale = (low.scale == 2 || low.scale == 4) ? low.scale : 2;
        // CAP must be created with the scaled (and aligned) low WxH; scaler selects plane from M2M
        if (!m_impl->configureSession(m_impl->lowSession, low, capNode, m_impl->deviceConfig.ispDevice, false, lowScale))
        {
            LOG(ERROR) << "Failed to start SNX low stream";
            m_impl->cleanupSession(m_impl->highSession);
            m_impl->running = false;
            return false;
        }
    }

    m_impl->running = true;
    // Best-effort: request an early IDR on both streams to prime clients
    (void)requestIDR(StreamKind::High);
    (void)requestIDR(StreamKind::Low);
    return true;
#else
    LOG(ERROR) << "SNX SDK support is not enabled at build time.";
    (void)high;
    (void)low;
    (void)devices;
    return false;
#endif
}

void SnxCodecController::stop()
{
#ifdef HAVE_SNX_SDK
    m_impl->running = false;
    m_impl->cleanupSession(m_impl->lowSession);
    m_impl->cleanupSession(m_impl->highSession);
#endif
}

bool SnxCodecController::isRunning() const
{
#ifdef HAVE_SNX_SDK
    return m_impl->running;
#else
    return false;
#endif
}

bool SnxCodecController::readFrame(StreamKind stream, std::vector<unsigned char> &buffer, timeval &presentation, bool &isKeyFrame)
{
#ifdef HAVE_SNX_SDK
    if (!isRunning())
    {
        return false;
    }

    Impl::Session &session = (stream == StreamKind::High) ? m_impl->highSession : m_impl->lowSession;

    if (!session.active)
    {
        return false;
    }

    int ret = snx_codec_read(&session.ctx);
    if (ret != 0)
    {
        if (ret == -EAGAIN || ret == -EINTR)
        {
            return false;
        }
        LOG(WARN) << "snx_codec_read returned " << ret;
        return false;
    }

    if (session.ctx.cap_index < 0 || session.ctx.cap_buffers == NULL)
    {
        LOG(WARN) << "SNX codec returned invalid buffer index";
        snx_codec_reset(&session.ctx);
        return false;
    }

    auto *raw = static_cast<unsigned char *>(session.ctx.cap_buffers[session.ctx.cap_index].start);
    size_t payload = static_cast<size_t>(std::max(0, session.ctx.cap_bytesused));
    const size_t capacity = session.ctx.cap_buffers[session.ctx.cap_index].length;
    if (payload > capacity)
    {
        payload = capacity;
    }

    buffer.assign(raw, raw + payload);
    presentation = session.ctx.timestamp;
    isKeyFrame = (session.ctx.flags & V4L2_BUF_FLAG_KEYFRAME) != 0;

    // Per-frame bitrate feedback for CBR (H.264 only)
    if (session.ctx.cap_bytesused > 0 && session.ctx.codec_fmt == V4L2_PIX_FMT_H264 && session.ctx.bit_rate > 0)
    {
        // Update QP based on actual frame size to maintain target bitrate
        session.ctx.qp = snx_codec_rc_update(&session.ctx, &session.rc);
    }

    if (snx_codec_reset(&session.ctx) != 0)
    {
        LOG(WARN) << "snx_codec_reset failed";
    }

    return !buffer.empty();
#else
    (void)stream;
    (void)buffer;
    (void)presentation;
    (void)isKeyFrame;
    return false;
#endif
}

int SnxCodecController::getPollFd(StreamKind stream) const
{
#ifdef HAVE_SNX_SDK
    const Impl::Session &session = (stream == StreamKind::High) ? m_impl->highSession : m_impl->lowSession;
    return session.ctx.codec_fd;
#else
    (void)stream;
    return -1;
#endif
}

std::size_t SnxCodecController::getMaxFrameSize(StreamKind stream) const
{
#ifdef HAVE_SNX_SDK
    const StreamParams &params = (stream == StreamKind::High) ? m_impl->highParams : m_impl->lowParams;
    return estimateFrameBudget(params);
#else
    (void)stream;
    return static_cast<size_t>(kDefaultFrameSizeBytes);
#endif
}

bool SnxCodecController::requestIDR(StreamKind stream)
{
#ifdef HAVE_SNX_SDK
    Impl::Session &session = (stream == StreamKind::High) ? m_impl->highSession : m_impl->lowSession;
    if (!session.active || session.ctx.codec_fd < 0) return false;
    // Try using V4L2 force keyframe control if available
#ifdef V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME
    struct v4l2_control ctrl;
    std::memset(&ctrl, 0, sizeof(ctrl));
    ctrl.id = V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME;
    ctrl.value = 1;
    if (ioctl(session.ctx.codec_fd, VIDIOC_S_CTRL, &ctrl) == 0)
    {
        return true;
    }
#endif
    // Some SDKs expose a helper in the middleware; try snx_codec_set_gop as a nudge (set same gop)
    if (session.ctx.gop > 0)
    {
        if (snx_codec_set_gop(&session.ctx) == 0)
        {
            return true;
        }
    }
    return false;
#else
    (void)stream;
    return false;
#endif
}
