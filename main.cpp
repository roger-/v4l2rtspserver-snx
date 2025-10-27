/* ---------------------------------------------------------------------------
** This software is in the public domain, furnished "as is", without technical
** support, and with no warranty, express or implied, as to its usefulness for
** any purpose.
**
** main.cpp
**
** V4L2 RTSP streamer
**
** H264 capture using V4L2
** RTSP using live555
**
** -------------------------------------------------------------------------*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <dirent.h>
#include <fcntl.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <getopt.h>
#include <memory>
#include <sstream>
#include <unistd.h>

// libv4l2
#include <linux/videodev2.h>

// project
#include "logger.h"

#include "V4l2Device.h"
#include "V4l2Output.h"

#include "V4l2RTSPServer.h"
#include "DeviceSourceFactory.h"
#include "H264_V4l2DeviceSource.h"
#include "V4L2DeviceSource.h"
#include "snx/SnxCodecController.h"
#include "snx/SnxDeviceInterface.h"

// -----------------------------------------
//    signal handler (graceful shutdown)
// -----------------------------------------
static volatile char quit = 0;
static void sighandler(int /*signum*/)
{
	// Set the watched variable; live555 event loop will exit promptly
	quit = 1;
}

// -------------------------------------------------------
//    split video,audio device
// -------------------------------------------------------
void decodeDevice(const std::string &device, std::string &videoDev, std::string &audioDev)
{
	std::istringstream is(device);
	getline(is, videoDev, ',');
	getline(is, audioDev);
}

std::string getDeviceName(const std::string &devicePath)
{
	std::string deviceName(devicePath);
	size_t pos = deviceName.find_last_of('/');
	if (pos != std::string::npos)
	{
		deviceName.erase(0, pos + 1);
	}
	return deviceName;
}

static bool parseWxHAtFps(const std::string &spec, unsigned &width, unsigned &height, unsigned &fps)
{
	unsigned w = 0;
	unsigned h = 0;
	unsigned f = 0;
	if (sscanf(spec.c_str(), "%ux%u@%u", &w, &h, &f) == 3)
	{
		width = w;
		height = h;
		fps = f;
		return true;
	}
	if (sscanf(spec.c_str(), "%ux%ux%u", &w, &h, &f) == 3)
	{
		width = w;
		height = h;
		fps = f;
		return true;
	}
	return false;
}

static std::string toLowerCopy(std::string value)
{
	for (size_t i = 0; i < value.size(); ++i)
	{
		unsigned char c = static_cast<unsigned char>(value[i]);
		value[i] = static_cast<char>(std::tolower(c));
	}
	return value;
}

// -----------------------------------------
//    entry point
// -----------------------------------------
int main(int argc, char **argv)
{
	// Allow large IDR frames without truncation (important for initial SPS/PPS/IDR bursts)
	OutPacketBuffer::maxSize = 2 * 1024 * 1024;

	// default parameters
	const char *dev_name = "/dev/video0,/dev/video0";
	unsigned int format = ~0;
	std::list<unsigned int> videoformatList;
	int width = 0;
	int height = 0;
	int queueSize = 5;
	int fps = 25;
	unsigned short rtspPort = 8554;
	unsigned short rtspOverHTTPPort = 0;
	bool multicast = false;
	int verbose = 0;
	std::string outputFile;
	V4l2IoType ioTypeIn = IOTYPE_MMAP;
	V4l2IoType ioTypeOut = IOTYPE_MMAP;
	int openflags = O_RDWR | O_NONBLOCK;
	std::string url = "unicast";
	std::string murl = "multicast";
	std::string tsurl = "ts";
	V4L2DeviceSource::CaptureMode captureMode = V4L2DeviceSource::CAPTURE_INTERNAL_THREAD;
	std::string maddr;
	bool repeatConfig = true;
	int timeout = 65;
	int defaultHlsSegment = 2;
	unsigned int hlsSegment = 0;
	std::string sslKeyCert;
	bool enableRTSPS = false;
	const char *realm = NULL;
	std::list<std::string> userPasswordList;
	std::string webroot;
#ifdef HAVE_ALSA
	int audioFreq = 44100;
	int audioNbChannels = 2;
	std::list<snd_pcm_format_t> audioFmtList;
	snd_pcm_format_t audioFmt = SND_PCM_FORMAT_UNKNOWN;
#endif
	struct SnxOptions
	{
		bool enabled;
		SnxCodecController::StreamParams hi;
		SnxCodecController::StreamParams lo;
		SnxCodecController::DeviceConfig devices;
		bool audioEnabled;
		std::string audioDevice;
		std::string audioEncoding;
		bool single;
	} snxOptions;

	snxOptions.enabled = true;
	snxOptions.audioEnabled = true;
	snxOptions.audioDevice = "hw:0,0";
	snxOptions.audioEncoding = "pcma";
	snxOptions.single = false;

	snxOptions.hi.width = 1920;
	snxOptions.hi.height = 1080;
	snxOptions.hi.fps = 10;
	snxOptions.hi.bitrate = 1024 * 1024;
	snxOptions.hi.gop = 20;
	snxOptions.hi.scale = 1;

	snxOptions.lo.scale = 2;
	snxOptions.lo.fps = 5;
	snxOptions.lo.bitrate = 512 * 1024;
	snxOptions.lo.gop = 5;

	snxOptions.devices.ispDevice = "/dev/video0";
	snxOptions.devices.m2mDevice = "/dev/video1";
	const char *defaultPort = getenv("PORT");
	if (defaultPort != NULL)
	{
		rtspPort = atoi(defaultPort);
	}

	enum
	{
		OPT_SNX = 1000,
		OPT_SNX_HI,
		OPT_SNX_HI_BITRATE,
		OPT_SNX_HI_GOP,
		OPT_SNX_LO_SCALE,
		OPT_SNX_LO_FPS,
		OPT_SNX_LO_BITRATE,
		OPT_SNX_LO_GOP,
		OPT_SNX_ISP_DEV,
		OPT_SNX_M2M_DEV,
		OPT_SNX_SINGLE,
		OPT_AUDIO_DEVICE,
		OPT_AUDIO_RTP,
		OPT_SNX_NO_AUDIO
	};

	static const struct option longOptions[] = {
		{"snx", no_argument, NULL, OPT_SNX},
		{"snx-hi", required_argument, NULL, OPT_SNX_HI},
		{"snx-hi-bitrate", required_argument, NULL, OPT_SNX_HI_BITRATE},
		{"snx-hi-gop", required_argument, NULL, OPT_SNX_HI_GOP},
		{"snx-lo-scale", required_argument, NULL, OPT_SNX_LO_SCALE},
		{"snx-lo-fps", required_argument, NULL, OPT_SNX_LO_FPS},
		{"snx-lo-bitrate", required_argument, NULL, OPT_SNX_LO_BITRATE},
		{"snx-lo-gop", required_argument, NULL, OPT_SNX_LO_GOP},
		{"snx-isp-dev", required_argument, NULL, OPT_SNX_ISP_DEV},
		{"snx-m2m-dev", required_argument, NULL, OPT_SNX_M2M_DEV},
		{"snx-single", no_argument, NULL, OPT_SNX_SINGLE},
		{"snx-no-audio", no_argument, NULL, OPT_SNX_NO_AUDIO},
		{"audio-dev", required_argument, NULL, OPT_AUDIO_DEVICE},
		{"audio-rtp", required_argument, NULL, OPT_AUDIO_RTP},
		{NULL, 0, NULL, 0}};

	// decode parameters
	int c = 0;
	while ((c = getopt_long(argc, argv,
					  "v::Q:O:b:"
								   "I:P:p:m::u:M::ct:S::x:X"
								   "R:U:"
								   "rwBsf::F:W:H:G:"
								   "A:C:a:"
					  "Vh",
					  longOptions, NULL)) != -1)
	{
		switch (c)
		{
		case OPT_SNX:
			snxOptions.enabled = true;
			break;
		case OPT_SNX_HI:
			if (!parseWxHAtFps(optarg, snxOptions.hi.width, snxOptions.hi.height, snxOptions.hi.fps))
			{
				LOG(ERROR) << "Invalid value for --snx-hi (expected WxH@fps): " << optarg;
				return 1;
			}
			if (snxOptions.hi.fps > 0)
			{
				snxOptions.hi.gop = snxOptions.hi.fps * 2;
			}
			break;
		case OPT_SNX_HI_BITRATE:
			snxOptions.hi.bitrate = strtoul(optarg, NULL, 10);
			break;
		case OPT_SNX_HI_GOP:
			snxOptions.hi.gop = strtoul(optarg, NULL, 10);
			break;
		case OPT_SNX_LO_SCALE:
			snxOptions.lo.scale = strtoul(optarg, NULL, 10);
			break;
	case OPT_SNX_LO_FPS:
		snxOptions.lo.fps = strtoul(optarg, NULL, 10);
		// For low FPS, use GOP = FPS (keyframe every second) to prevent VLC freezing
		snxOptions.lo.gop = snxOptions.lo.fps > 0 ? snxOptions.lo.fps : snxOptions.lo.gop;
		break;
	case OPT_SNX_LO_BITRATE:
		snxOptions.lo.bitrate = strtoul(optarg, NULL, 10);
		break;
	case OPT_SNX_LO_GOP:
		snxOptions.lo.gop = strtoul(optarg, NULL, 10);
		break;
	case OPT_SNX_ISP_DEV:
			snxOptions.devices.ispDevice = optarg;
			break;
		case OPT_SNX_M2M_DEV:
			snxOptions.devices.m2mDevice = optarg;
			break;
		case OPT_SNX_SINGLE:
			snxOptions.single = true;
			break;
		case OPT_AUDIO_DEVICE:
			snxOptions.audioDevice = optarg;
			break;
		case OPT_AUDIO_RTP:
			snxOptions.audioEncoding = toLowerCopy(optarg);
			break;
		case OPT_SNX_NO_AUDIO:
			snxOptions.audioEnabled = false;
			break;
		case 'v':
			verbose = 1;
			if (optarg && *optarg == 'v')
				verbose++;
			break;
		case 'Q':
			queueSize = atoi(optarg);
			break;
		case 'O':
			outputFile = optarg;
			break;
		case 'b':
			webroot = optarg;
			break;

		// RTSP/RTP
		case 'I':
			ReceivingInterfaceAddr = inet_addr(optarg);
			break;
		case 'P':
			rtspPort = atoi(optarg);
			break;
		case 'p':
			rtspOverHTTPPort = atoi(optarg);
			break;
		case 'u':
			url = optarg;
			break;
		case 'm':
			multicast = true;
			murl = optarg ? optarg : murl;
			break;
		case 'M':
			multicast = true;
			maddr = optarg ? optarg : maddr;
			break;
		case 'c':
			repeatConfig = false;
			break;
		case 't':
			timeout = atoi(optarg);
			break;
		case 'S':
			hlsSegment = optarg ? atoi(optarg) : defaultHlsSegment;
			break;
#ifndef NO_OPENSSL
		case 'x':
			sslKeyCert = optarg;
			break;
		case 'X':
			enableRTSPS = true;
			break;
#endif

		// users
		case 'R':
			realm = optarg;
			break;
		case 'U':
			userPasswordList.push_back(optarg);
			break;

		// V4L2
		case 'r':
			ioTypeIn = IOTYPE_READWRITE;
			break;
		case 'w':
			ioTypeOut = IOTYPE_READWRITE;
			break;
		case 'B':
			openflags = O_RDWR;
			break;
		case 's':
			captureMode = V4L2DeviceSource::CAPTURE_LIVE555_THREAD;
			break;
		case 'f':
			format = V4l2Device::fourcc(optarg);
			if (format)
			{
				videoformatList.push_back(format);
			};
			break;
		case 'F':
			fps = atoi(optarg);
			break;
		case 'W':
			width = atoi(optarg);
			break;
		case 'H':
			height = atoi(optarg);
			break;
		case 'G':
			sscanf(optarg, "%dx%dx%d", &width, &height, &fps);
			break;

			// ALSA
#ifdef HAVE_ALSA
		case 'A':
			audioFreq = atoi(optarg);
			break;
		case 'C':
			audioNbChannels = atoi(optarg);
			break;
		case 'a':
			audioFmt = V4l2RTSPServer::decodeAudioFormat(optarg);
			if (audioFmt != SND_PCM_FORMAT_UNKNOWN)
			{
				audioFmtList.push_back(audioFmt);
			};
			break;
#endif

		// version
		case 'V':
			std::cout << VERSION << std::endl;
			exit(0);
			break;

		// help
		case 'h':
		default:
		{
			std::cout << argv[0] << " [-v[v]] [-Q queueSize] [-O file]" << std::endl;
			std::cout << "\t          [-I interface] [-P RTSP port] [-p RTSP/HTTP port] [-m multicast url] [-u unicast url] [-M multicast addr] [-c] [-t timeout] [-T] [-S[duration]]" << std::endl;
			std::cout << "\t          [-r] [-w] [-s] [-f[format] [-W width] [-H height] [-F fps] [device] [device]" << std::endl;
			std::cout << "\t -v               : verbose" << std::endl;
			std::cout << "\t -vv              : very verbose" << std::endl;
			std::cout << "\t -Q <length>      : Number of frame queue  (default " << queueSize << ")" << std::endl;
			std::cout << "\t -O <output>      : Copy captured frame to a file or a V4L2 device" << std::endl;
			std::cout << "\t -b <webroot>     : path to webroot" << std::endl;

			std::cout << "\t RTSP/RTP options" << std::endl;
			std::cout << "\t -I <addr>        : RTSP interface (default autodetect)" << std::endl;
			std::cout << "\t -P <port>        : RTSP port (default " << rtspPort << ")" << std::endl;
			std::cout << "\t -p <port>        : RTSP over HTTP port (default " << rtspOverHTTPPort << ")" << std::endl;
			std::cout << "\t -U <user>:<pass> : RTSP user and password" << std::endl;
			std::cout << "\t -R <realm>       : use md5 password 'md5(<username>:<realm>:<password>')" << std::endl;
			std::cout << "\t -u <url>         : unicast url (default " << url << ")" << std::endl;
			std::cout << "\t -m <url>         : multicast url (default " << murl << ")" << std::endl;
			std::cout << "\t -M <addr>        : multicast group:port (default is random_address:20000)" << std::endl;
			std::cout << "\t -c               : don't repeat config (default repeat config before IDR frame)" << std::endl;
			std::cout << "\t -t <timeout>     : RTCP expiration timeout in seconds (default " << timeout << ")" << std::endl;
			std::cout << "\t -S[<duration>]   : enable HLS & MPEG-DASH with segment duration  in seconds (default " << defaultHlsSegment << ")" << std::endl;
#ifndef NO_OPENSSL
			std::cout << "\t -x <sslkeycert>  : enable SRTP" << std::endl;
			std::cout << "\t -X               : enable RTSPS" << std::endl;
#endif

			std::cout << "\t V4L2 options" << std::endl;
			std::cout << "\t -r               : V4L2 capture using read interface (default use memory mapped buffers)" << std::endl;
			std::cout << "\t -w               : V4L2 capture using write interface (default use memory mapped buffers)" << std::endl;
			std::cout << "\t -B               : V4L2 capture using blocking mode (default use non-blocking mode)" << std::endl;
			std::cout << "\t -s               : V4L2 capture using live555 mainloop (default use a reader thread)" << std::endl;
			std::cout << "\t -f               : V4L2 capture using current capture format (-W,-H,-F are ignored)" << std::endl;
			std::cout << "\t -f<format>       : V4L2 capture using format (-W,-H,-F are used)" << std::endl;
			std::cout << "\t -W <width>       : V4L2 capture width (default " << width << ")" << std::endl;
			std::cout << "\t -H <height>      : V4L2 capture height (default " << height << ")" << std::endl;
			std::cout << "\t -F <fps>         : V4L2 capture framerate (default " << fps << ")" << std::endl;
			std::cout << "\t -G <w>x<h>[x<f>] : V4L2 capture format (default " << width << "x" << height << "x" << fps << ")" << std::endl;

#ifdef HAVE_ALSA
			std::cout << "\t ALSA options" << std::endl;
			std::cout << "\t -A freq          : ALSA capture frequency and channel (default " << audioFreq << ")" << std::endl;
			std::cout << "\t -C channels      : ALSA capture channels (default " << audioNbChannels << ")" << std::endl;
			std::cout << "\t -a fmt           : ALSA capture audio format (default S16_BE)" << std::endl;
#endif

			std::cout << "\t Devices :" << std::endl;
			std::cout << "\t [V4L2 device][,ALSA device] : V4L2 capture device or/and ALSA capture device (default " << dev_name << ")" << std::endl;

			// SNX options help
#ifdef HAVE_SNX_SDK
			std::cout << "\n\t SNX options (enabled)" << std::endl;
#else
			std::cout << "\n\t SNX options (disabled at build time)" << std::endl;
#endif
			std::cout << "\t --snx                 : enable Sonix dual-stream mode" << std::endl;
			std::cout << "\t --snx-hi WxH@fps      : high stream resolution and fps (default: 1920x1080@10)" << std::endl;
			std::cout << "\t --snx-hi-bitrate N    : high stream bitrate in bits/sec (default: 1048576)" << std::endl;
			std::cout << "\t --snx-hi-gop N        : high stream GOP in frames (default: 20)" << std::endl;
			std::cout << "\t --snx-lo-scale {1|2|4}: low stream scale factor from high (default: 2)" << std::endl;
			std::cout << "\t --snx-lo-fps N        : low stream fps, <= high fps (default: 5)" << std::endl;
			std::cout << "\t --snx-lo-bitrate N    : low stream bitrate in bits/sec (default: 524288)" << std::endl;
			std::cout << "\t --snx-lo-gop N        : low stream GOP in frames (default: 5)" << std::endl;
			std::cout << "\t --snx-isp-dev PATH    : ISP device (default: /dev/video0)" << std::endl;
			std::cout << "\t --snx-m2m-dev PATH    : Codec M2M device (default: /dev/video1)" << std::endl;
			std::cout << "\t --snx-single          : start only high (M2M) stream, disable low/CAP" << std::endl;
			std::cout << "\t --snx-no-audio        : disable audio in SNX mode" << std::endl;
			std::cout << "\t --audio-dev NAME      : ALSA device name (default: hw:0,0)" << std::endl;
			std::cout << "\t --audio-rtp pcma|pcmu : audio RTP payload, G.711 A-law or mu-law (default: pcma)" << std::endl;
			exit(0);
		}
		}
	}
	if (snxOptions.enabled)
	{
		if ((snxOptions.hi.width == 0) || (snxOptions.hi.height == 0) || (snxOptions.hi.fps == 0))
		{
			LOG(ERROR) << "SNX mode requires --snx-hi to specify width, height and fps.";
			return 1;
		}
		if (!snxOptions.single && (snxOptions.lo.scale != 1) && (snxOptions.lo.scale != 2) && (snxOptions.lo.scale != 4))
		{
			LOG(ERROR) << "SNX low stream scale must be one of {1,2,4}.";
			return 1;
		}
		if (snxOptions.single)
		{
			snxOptions.lo.width = 0;
			snxOptions.lo.height = 0;
			snxOptions.lo.fps = 0;
			snxOptions.lo.scale = 0;
		}
		else
		{
			snxOptions.lo.width = snxOptions.hi.width / snxOptions.lo.scale;
			snxOptions.lo.height = snxOptions.hi.height / snxOptions.lo.scale;
			if ((snxOptions.lo.width == 0) || (snxOptions.lo.height == 0))
			{
				LOG(ERROR) << "SNX low stream resolution computed to zero. Adjust --snx-hi or --snx-lo-scale.";
				return 1;
			}
			// Align low WxH to multiples of 16 to satisfy encoder requirements on some SDK drops
			auto align16 = [](unsigned v) { return (v + 15u) & ~15u; };
			unsigned alignedW = align16(snxOptions.lo.width);
			unsigned alignedH = align16(snxOptions.lo.height);
			if (alignedW != (unsigned)snxOptions.lo.width || alignedH != (unsigned)snxOptions.lo.height)
			{
				LOG(WARN) << "SNX: aligning low size from " << snxOptions.lo.width << "x" << snxOptions.lo.height
						   << " to " << alignedW << "x" << alignedH;
				snxOptions.lo.width = alignedW;
				snxOptions.lo.height = alignedH;
			}
			if (snxOptions.lo.fps == 0)
			{
				snxOptions.lo.fps = snxOptions.hi.fps;
			}
			if (snxOptions.lo.fps > snxOptions.hi.fps)
			{
				LOG(ERROR) << "SNX requires low fps <= high fps.";
				return 1;
			}
		}
		if ((snxOptions.hi.gop == 0) && (snxOptions.hi.fps > 0))
		{
			snxOptions.hi.gop = snxOptions.hi.fps * 2;
		}
		if ((snxOptions.lo.gop == 0) && (snxOptions.lo.fps > 0))
		{
			// For low FPS streams, use GOP = FPS (keyframe every second) to prevent freezing
			// At 5fps with GOP=10 (2 seconds), VLC freezes waiting for keyframes
			snxOptions.lo.gop = snxOptions.lo.fps; // Keyframe every ~1 second
		}
		// Couple scaler between high and low: if a low stream is requested with scale 2 or 4,
		// the high (M2M) pipeline must enable the same scale so CAP can attach to that plane.
		if (!snxOptions.single)
		{
			unsigned s = snxOptions.lo.scale;
			if (s == 2 || s == 4)
			{
				snxOptions.hi.scale = s;
			}
			else
			{
				snxOptions.hi.scale = 1;
			}
		}
		else
		{
			snxOptions.hi.scale = 1;
		}
		// Keep computed low WxH consistent with chosen high scale
		if (!snxOptions.single && snxOptions.hi.scale != 0)
		{
			snxOptions.lo.width = snxOptions.hi.width / snxOptions.hi.scale;
			snxOptions.lo.height = snxOptions.hi.height / snxOptions.hi.scale;
			// Re-apply alignment to multiples of 16
			auto align16b = [](unsigned v) { return (v + 15u) & ~15u; };
			snxOptions.lo.width = align16b(snxOptions.lo.width);
			snxOptions.lo.height = align16b(snxOptions.lo.height);
		}
#ifdef HAVE_ALSA
		if (snxOptions.audioEnabled)
		{
			// Force SNX audio to G.711 friendly settings: 8000 Hz, mono
			audioFreq = 8000;
			audioNbChannels = 1;
			audioFmtList.clear();
			if (snxOptions.audioEncoding == "pcmu")
			{
				audioFmtList.push_back(SND_PCM_FORMAT_MU_LAW);
			}
			else if (snxOptions.audioEncoding == "pcma")
			{
				audioFmtList.push_back(SND_PCM_FORMAT_A_LAW);
			}
			else
			{
				LOG(ERROR) << "Unsupported --audio-rtp setting for SNX: " << snxOptions.audioEncoding;
				return 1;
			}
			LOG(NOTICE) << "SNX Audio: " << (snxOptions.audioEncoding == "pcmu" ? "PCMU" : "PCMA")
					  << "/" << audioFreq << "/" << audioNbChannels
					  << ", ALSA device='" << snxOptions.audioDevice << "'";
		}
#endif
	}

	std::list<std::string> devList;
	while (optind < argc)
	{
		devList.push_back(argv[optind]);
		optind++;
	}
	if (devList.empty())
	{
		devList.push_back(dev_name);
	}

	// default format tries
	if ((videoformatList.empty()) && (format != 0))
	{
		videoformatList.push_back(V4L2_PIX_FMT_HEVC);
		videoformatList.push_back(V4L2_PIX_FMT_H264);
		videoformatList.push_back(V4L2_PIX_FMT_MJPEG);
		videoformatList.push_back(V4L2_PIX_FMT_JPEG);
		videoformatList.push_back(V4L2_PIX_FMT_NV12);
	}

#ifdef HAVE_ALSA
	// default audio format tries
	if (audioFmtList.empty())
	{
		audioFmtList.push_back(SND_PCM_FORMAT_S16_LE);
		audioFmtList.push_back(SND_PCM_FORMAT_S16_BE);
	}
#endif

	// init logger
	initLogger(verbose);
	LOG(NOTICE) << "Version: " << VERSION << " live555 version:" << LIVEMEDIA_LIBRARY_VERSION_STRING;

	// create RTSP server
	V4l2RTSPServer rtspServer(rtspPort, rtspOverHTTPPort, timeout, hlsSegment, userPasswordList, realm, webroot, sslKeyCert, enableRTSPS);
	if (!rtspServer.available())
	{
		LOG(ERROR) << "Failed to create RTSP server: " << rtspServer.getResultMsg();
	}
	else
	{
		// decode multicast info
		struct in_addr destinationAddress;
		unsigned short rtpPortNum;
		unsigned short rtcpPortNum;
		rtspServer.decodeMulticastUrl(maddr, destinationAddress, rtpPortNum, rtcpPortNum);

		if (snxOptions.enabled)
		{
			UsageEnvironment &env = *rtspServer.env();
			
			auto controller = std::make_shared<SnxCodecController>();
			if (!controller->start(snxOptions.hi, snxOptions.lo, snxOptions.devices))
			{
				LOG(ERROR) << "Unable to start SNX codec controller.";
				return 1;
			}

			// We'll create V4L2DeviceSource-backed replicators per-stream and keep them for cleanup
			StreamReplicator *hiReplForSub = NULL;
			StreamReplicator *loReplForSub = NULL;

#ifdef HAVE_ALSA
		StreamReplicator *audioReplicator = NULL;
		if (snxOptions.audioEnabled)
		{
			audioReplicator = rtspServer.CreateAudioReplicator(
				snxOptions.audioDevice, audioFmtList, audioFreq, audioNbChannels, verbose,
				queueSize, captureMode);
			if (audioReplicator == NULL)
			{
				LOG(WARN) << "Failed to create audio replicator; continuing without audio";
			}
		}
#else
		StreamReplicator *audioReplicator = NULL;
#endif			// Reuse generic UnicastServerMediaSubsession path via a V4L2DeviceSource-compatible adapter
			{
				// Create a V4L2DeviceSource using our SNX adapter; repeatConfig=true, keepMarker=true for H264
				DeviceInterface *hiDev = new SnxDeviceInterface(controller, SnxCodecController::High, snxOptions.hi.width, snxOptions.hi.height);
				// Do not keep Annex-B start codes when feeding H264VideoStreamDiscreteFramer
				V4L2DeviceSource *hiV4L2 = H264_V4L2DeviceSource::createNew(env, hiDev, -1, queueSize, V4L2DeviceSource::CAPTURE_INTERNAL_THREAD, /*repeatConfig*/true, /*keepMarker*/false);
				if (hiV4L2 == NULL)
				{
					LOG(ERROR) << "Failed to create SNX high V4L2DeviceSource.";
					controller->stop();
					return 1;
				}
				// Prime aux-SDP (SPS/PPS) before SDP generation to help VLC/FFmpeg at startup
				{
					const int kMaxIters = 50; // ~500ms
					int iters = 0;
					while (hiV4L2->getAuxLine().empty() && iters < kMaxIters)
					{
						usleep(10 * 1000);
						iters++;
					}
					if (!hiV4L2->getAuxLine().empty())
					{
						LOG(DEBUG) << "SNX high aux ready after " << iters << " iterations";
					}
				}
				// Keep the input source alive across client disconnects
				hiReplForSub = StreamReplicator::createNew(env, hiV4L2, False);
				if (hiReplForSub == NULL)
				{
					LOG(ERROR) << "Failed to create SNX high StreamReplicator.";
					controller->stop();
					Medium::close(hiV4L2);
					return 1;
				}
			}

			ServerMediaSession *smsHigh = rtspServer.AddUnicastSession("high", hiReplForSub, snxOptions.audioEnabled ? audioReplicator : NULL);
			if (smsHigh)
			{
				std::string urlHigh = rtspServer.getRtspUrl(smsHigh);
				LOG(NOTICE) << "RTSP High URL: " << (urlHigh.empty() ? std::string("(unavailable)") : urlHigh);
			}

			ServerMediaSession *smsLow = NULL;
			if (!snxOptions.single)
			{
				DeviceInterface *loDev = new SnxDeviceInterface(controller, SnxCodecController::Low, snxOptions.lo.width, snxOptions.lo.height);
				// Do not keep Annex-B start codes when feeding H264VideoStreamDiscreteFramer
				V4L2DeviceSource *loV4L2 = H264_V4L2DeviceSource::createNew(env, loDev, -1, queueSize, V4L2DeviceSource::CAPTURE_INTERNAL_THREAD, /*repeatConfig*/true, /*keepMarker*/false);
				if (loV4L2 == NULL)
				{
					LOG(ERROR) << "Failed to create SNX low V4L2DeviceSource.";
					controller->stop();
					// Also cleanup high branch
					Medium::close(smsHigh);
					Medium::close(hiReplForSub);
					return 1;
				}
				// Prime aux-SDP for low stream as well
				{
					const int kMaxIters = 50;
					int iters = 0;
					while (loV4L2->getAuxLine().empty() && iters < kMaxIters)
					{
						usleep(10 * 1000);
						iters++;
					}
					if (!loV4L2->getAuxLine().empty())
					{
						LOG(DEBUG) << "SNX low aux ready after " << iters << " iterations";
					}
				}
				// Keep the input source alive across client disconnects
				loReplForSub = StreamReplicator::createNew(env, loV4L2, False);
				if (loReplForSub == NULL)
				{
					LOG(ERROR) << "Failed to create SNX low StreamReplicator.";
					controller->stop();
					Medium::close(loV4L2);
					// Also cleanup high branch
					Medium::close(smsHigh);
					Medium::close(hiReplForSub);
					return 1;
				}

				smsLow = rtspServer.AddUnicastSession("low", loReplForSub, snxOptions.audioEnabled ? audioReplicator : NULL);
				if (smsLow)
				{
				std::string urlLow = rtspServer.getRtspUrl(smsLow);
				LOG(NOTICE) << "RTSP Low URL:  " << (urlLow.empty() ? std::string("(unavailable)") : urlLow);
			}
		}
		
		signal(SIGINT, sighandler);
		signal(SIGTERM, sighandler);
		signal(SIGQUIT, sighandler);
		signal(SIGHUP, sighandler);
		rtspServer.eventLoop((char*)&quit);
		
		LOG(NOTICE) << "Exiting....";
		
		// Step 1: Stop the SNX controller first (stops producing frames)
		LOG(DEBUG) << "Stopping SNX controller...";
		if (controller) {
			controller->stop();
		}
		
		// Step 2: Request capture threads to stop (sets atomic flag they check in loop)
		LOG(DEBUG) << "Requesting capture threads to stop...";
		auto stopSource = [](StreamReplicator* repl){
			if (!repl) return;
			FramedSource* fs = repl->inputSource();
			V4L2DeviceSource* vsrc = dynamic_cast<V4L2DeviceSource*>(fs);
			if (vsrc) vsrc->requestStop();
		};
		stopSource(hiReplForSub);
		stopSource(loReplForSub);
#ifdef HAVE_ALSA
		stopSource(audioReplicator);
#endif
		
		// Give threads time to exit (no more frames coming, stop flag set)
		LOG(DEBUG) << "Waiting for threads to exit...";
		usleep(500000); // 500ms - increased for safety
		
		// Step 3: Close sessions first (they reference the replicators)
		LOG(DEBUG) << "Closing RTSP sessions...";
		if (smsHigh) {
			LOG(DEBUG) << "Closing high session...";
			rtspServer.RemoveSession(smsHigh);
			smsHigh = NULL;
			LOG(DEBUG) << "High session closed.";
		}
		if (smsLow) {
			LOG(DEBUG) << "Closing low session...";
			rtspServer.RemoveSession(smsLow);
			smsLow = NULL;
			LOG(DEBUG) << "Low session closed.";
		}
		
		// Step 4: Close replicators (joins threads - should be quick since they're stopping)
		LOG(DEBUG) << "Closing video replicators...";
		if (hiReplForSub) {
			LOG(DEBUG) << "Closing high replicator...";
			Medium::close(hiReplForSub);
			hiReplForSub = NULL;
			LOG(DEBUG) << "High replicator closed.";
		}
		if (loReplForSub) {
			LOG(DEBUG) << "Closing low replicator...";
			Medium::close(loReplForSub);
			loReplForSub = NULL;
			LOG(DEBUG) << "Low replicator closed.";
		}
		
		// Step 5: Close audio replicator last
		LOG(DEBUG) << "Closing audio replicator...";
		if (audioReplicator) {
			Medium::close(audioReplicator);
			audioReplicator = NULL;
			LOG(DEBUG) << "Audio replicator closed.";
		}
		
		LOG(NOTICE) << "Cleanup complete.";
		return 0;
	}

		std::list<V4l2Output *> outList;
		int nbSource = 0;
		std::list<std::string>::iterator devIt;
		for (devIt = devList.begin(); devIt != devList.end(); ++devIt)
		{
			std::string deviceName(*devIt);

			std::string videoDev;
			std::string audioDev;
			decodeDevice(deviceName, videoDev, audioDev);

			std::string baseUrl;
			std::string output(outputFile);
			if (devList.size() > 1)
			{
				baseUrl = getDeviceName(videoDev);
				baseUrl.append("_");
				// output is not compatible with multiple device
				output.assign("");
			}

			V4l2Output *out = NULL;
			V4L2DeviceParameters inParam(videoDev.c_str(), videoformatList, width, height, fps, ioTypeIn, openflags);
			StreamReplicator *videoReplicator = rtspServer.CreateVideoReplicator(
				inParam,
				queueSize, captureMode, repeatConfig,
				output, ioTypeOut, out);
			if (out != NULL)
			{
				outList.push_back(out);
			}

			// Init Audio Capture
			StreamReplicator *audioReplicator = NULL;
#ifdef HAVE_ALSA
			audioReplicator = rtspServer.CreateAudioReplicator(
				audioDev, audioFmtList, audioFreq, audioNbChannels, verbose,
				queueSize, captureMode);
#endif

			// Create Multicast Session
			if (multicast)
			{
				ServerMediaSession *sms = rtspServer.AddMulticastSession(baseUrl + murl, destinationAddress, rtpPortNum, rtcpPortNum, videoReplicator, audioReplicator);
				if (sms)
				{
					nbSource += sms->numSubsessions();
				}
			}

			// Create HLS Session
			if (hlsSegment > 0)
			{
				ServerMediaSession *sms = rtspServer.AddHlsSession(baseUrl + tsurl, hlsSegment, videoReplicator, audioReplicator);
				if (sms)
				{
					nbSource += sms->numSubsessions();
				}
			}

			// Create Unicast Session
			ServerMediaSession *sms = rtspServer.AddUnicastSession(baseUrl + url, videoReplicator, audioReplicator);
			if (sms)
			{
				nbSource += sms->numSubsessions();
			}
		}

		if (nbSource > 0)
		{
			// main loop
			signal(SIGINT, sighandler);
			signal(SIGTERM, sighandler);
			signal(SIGQUIT, sighandler);
			signal(SIGHUP, sighandler);
			rtspServer.eventLoop((char*)&quit);
			// Proactively stop capture threads behind video replicators
			// Note: in the generic path, we don't keep direct handles to replicators per-session.
			// They will be closed when sessions are closed; sources check m_stop too, so the shutdown proceeds promptly.
			LOG(NOTICE) << "Exiting....";
		}

		while (!outList.empty())
		{
			V4l2Output *out = outList.back();
			delete out;
			outList.pop_back();
		}
	}

	return 0;
}
