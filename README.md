v4l2rtspserver for SONiX SN98600
====================

This is a minimally modified version of [v4l2rtspserver](https://github.com/mpromonet/v4l2rtspserver) for the XiaoFang WiFi Camera (with SONiX SN98600 SoC). Unlike the outdated one in Fang Hacks, this is based on the latest v4l2rtspserver/Live555, so it should be more stable. It also supports dual streams (e.g. low/high resolution), audio, authentication, etc. HTTP/HLS may work, it hasn't been tested.

To use, first install [Fang Hacks](https://github.com/samtap/fang-hacks) on your device (I use [this fork](https://github.com/davidjb/fang-hacks)) to get SSH access and copy the pre-compiled `v4l2rtspserver` binary to your device. I recommend disabling cloud access (probably doesn't work now anyway) and modifying the startup scripts.

# Building

If you want to build from scratch, install a Dockerized SDK and do this:

```bash
cd v4l2rtspserver-snx
mkdir -p build-snx
cd build-snx
SNX_SDK_ROOT=/opt/sonix/sonix9600/snx_sdk cmake -DCMAKE_TOOLCHAIN_FILE=../snx/snx98600-toolchain.cmake -DSTATIC_BUILD=ON ..
make
```

Note that the SN98600 is very old and doesn't support modern C++, so several hacks were needed to make it compile. AI was used to help with the port and fix some bugs.

---

[![CircleCI](https://circleci.com/gh/mpromonet/v4l2rtspserver.svg?style=shield)](https://circleci.com/gh/mpromonet/v4l2rtspserver)
[![CirusCI](https://api.cirrus-ci.com/github/mpromonet/v4l2rtspserver.svg?branch=master)](https://cirrus-ci.com/github/mpromonet/v4l2rtspserver)
[![Snap Status](https://snapcraft.io//v4l2-rtspserver/badge.svg)](https://snapcraft.io/v4l2-rtspserver)
[![GithubCI](https://github.com/mpromonet/v4l2rtspserver/workflows/C/C++%20CI/badge.svg)](https://github.com/mpromonet/v4l2rtspserver/actions)

[![Codacy Badge](https://app.codacy.com/project/badge/Grade/4602f447d1c1408d865ebb4ef68f12f1)](https://app.codacy.com/gh/mpromonet/v4l2rtspserver/dashboard?utm_source=gh&utm_medium=referral&utm_content=&utm_campaign=Badge_grade)
[![Coverity Scan Build Status](https://scan.coverity.com/projects/4644/badge.svg)](https://scan.coverity.com/projects/4644)
[![Coverage Status](https://coveralls.io/repos/github/mpromonet/v4l2rtspserver/badge.svg?branch=master)](https://coveralls.io/github/mpromonet/v4l2rtspserver?branch=master)
[![OpenSSF Best Practices](https://www.bestpractices.dev/projects/8853/badge)](https://www.bestpractices.dev/projects/8853)

[![Release](https://img.shields.io/github/release/mpromonet/v4l2rtspserver.svg)](https://github.com/mpromonet/v4l2rtspserver/releases/latest)
[![Download](https://img.shields.io/github/downloads/mpromonet/v4l2rtspserver/total.svg)](https://github.com/mpromonet/v4l2rtspserver/releases/latest)
[![Docker Pulls](https://img.shields.io/docker/pulls/mpromonet/v4l2rtspserver.svg)](https://hub.docker.com/r/mpromonet/v4l2rtspserver/)
[![OpenWrt 24.10 x86_64 package](https://repology.org/badge/version-for-repo/openwrt_24_10_x86_64/v4l2rtspserver.svg)](https://repology.org/project/v4l2rtspserver/versions)

v4l2rtspserver
====================

This is an streamer feed from :
 - an Video4Linux device that support H264, HEVC, JPEG, VP8 or VP9 capture.
 - an ALSA device that support PCM S16_BE, S16_LE, S32_BE or S32_LE
 
The RTSP server support :
- RTP/UDP unicast
- RTP/UDP multicast
- RTP/TCP
- RTP/RTSP/HTTP

The HTTP server support (available using -S option for capture format that could be muxed in Transport Stream):
- HLS
- MPEG-DASH

Dependencies
------------
 - liblivemedia-dev [License LGPL](http://www.live555.com/liveMedia/) > live.2012.01.07 (need StreamReplicator)
 - libv4l2cpp [Unlicense](https://github.com/mpromonet/libv4l2cpp/blob/master/LICENSE)
 - liblog4cpp5-dev  [License LGPL](http://log4cpp.sourceforge.net/#license) (optional)
If liblog4cpp5-dev is not present, a simple log using std::cout is used.
 - libasound2-dev Licence LGPL (optional)
If libasound2-dev is not present in the build environment, there will have no audio support.
 - libssl-dev (optional)
If libssl-dev is not present rtsps/srtp will not be available

Usage
-----
	./v4l2rtspserver [-v[v]] [-Q queueSize] [-O file] \
			       [-I interface] [-P RTSP port] [-p RTSP/HTTP port] [-m multicast url] [-u unicast url] [-M multicast addr] [-c] [-t timeout] \
			       [-r] [-s] [-W width] [-H height] [-F fps] [device1] [device2]
		 -v       : verbose
		 -vv      : very verbose
		 -Q length: Number of frame queue  (default 10)
		 -O output: Copy captured frame to a file or a V4L2 device
		 
		 RTSP options :
		 -I addr  : RTSP interface (default autodetect)
		 -P port  : RTSP port (default 8554)
		 -p port  : RTSP over HTTP port (default 0)
		 -U user:password : RTSP user and password
		 -R realm  : use md5 password 'md5(<username>:<realm>:<password>')
		 -u url   : unicast url (default unicast)
		 -m url   : multicast url (default multicast)
		 -M addr  : multicast group:port (default is random_address:20000)
		 -c       : don't repeat config (default repeat config before IDR frame)
		 -t secs  : RTCP expiration timeout (default 65)
		 -S[secs] : HTTP segment duration (enable HLS & MPEG-DASH)
		 -x <sslkeycert>  : enable SRTP
		 -X               : enable RSTPS
 
		 V4L2 options :
		 -r       : V4L2 capture using read interface (default use memory mapped buffers)
		 -w       : V4L2 capture using write interface (default use memory mapped buffers)
		 -B       : V4L2 capture using blocking mode (default use non-blocking mode)
		 -s       : V4L2 capture using live555 mainloop (default use a separated reading thread)
		 -f       : V4L2 capture using current capture format (-W,-H are ignored)
		 -fformat : V4L2 capture using format (-W,-H are used)
		 -W width : V4L2 capture width (default 640)
		 -H height: V4L2 capture height (default 480)
		 -F fps   : V4L2 capture framerate (default 25, 0 disable setting framerate)
		 -G <w>x<h>[x<f>] : V4L2 capture format (default 0x0x25)
		 
		 ALSA options :
		 -A freq    : ALSA capture frequency and channel (default 44100)
		 -C channels: ALSA capture channels (default 2)
		 -a fmt     : ALSA capture audio format (default S16_LE)
		 
		 device   : V4L2 capture device and/or ALSA device (default /dev/video0)

When audio support is not present, ALSA options are not printed running with `-h` argument.

Authentification is enable when almost one user is defined. You can configure credentials :
 * using plain text password: 
 
       -U foo:bar -U admin:admin
 * using md5 password: 
 
       -R myrealm -U foo:$(echo -n foo:myrealm:bar | md5sum | cut -d- -f1) -U admin:$(echo -n admin:myrealm:admin | md5sum | cut -d- -f1)

It is possible to compose the RTSP session is different ways :
 * v4l2rtspserver /dev/video0              : one RTSP session with RTP video capturing V4L2 device /dev/video0
 * v4l2rtspserver ,default                 : one RTSP session with RTP audio capturing ALSA device default
 * v4l2rtspserver /dev/video0,default      : one RTSP session with RTP audio and RTP video
 * v4l2rtspserver /dev/video0 ,default     : two RTSP sessions first one with RTP video and second one with RTP audio
 * v4l2rtspserver /dev/video0 /dev/video1  : two RTSP sessions with an RTP video
 * v4l2rtspserver /dev/video0,/dev/video0  : one RTSP session with RTP audio and RTP video (ALSA device associatd with the V4L2 device)

Build
------- 
- Build  

		cmake . && make

	If live555 is not installed it will download it from live555.com and compile it. If asound is not installed, ALSA will be disabled.  
	If it still not work you will need to read Makefile.  

- Install (optional) 

		sudo make install

- Packaging  (optional)

		cpack .

Using Raspberry Pi Camera
------------------------- 
This RTSP server works with Raspberry Pi camera using :
- the opensource V4L2 driver bcm2835-v4l2

	sudo modprobe -v bcm2835-v4l2
	

Using v4l2loopback
----------------------- 
For camera providing uncompress format [v4l2tools](https://github.com/mpromonet/v4l2tools) can compress the video to an intermediate virtual V4L2 device [v4l2loopback](https://github.com/umlaeute/v4l2loopback):

	/dev/video0 (camera device)-> v4l2compress -> /dev/video10 (v4l2loopback device) -> v4l2rtspserver

This workflow could be set using :

	modprobe v4l2loopback video_nr=10
	v4l2compress -fH264 /dev/video0 /dev/video10 &
	v4l2rtspserver /dev/video10 &


Playing HTTP streams
-----------------------
When v4l2rtspserver is started with '-S' arguments it also give access to streams through HTTP.  
These streams could be played :

	* for MPEG-DASH with :   
           MP4Client http://..../unicast.mpd   
	   
	* for HLS with :  
           vlc http://..../unicast.m3u8  
           gstreamer-launch-1.0 playbin uri=http://.../unicast.m3u8  

It is now possible to play HLS url directly from browser :

 * using Firefox installing [Native HLS addons](https://addons.mozilla.org/en-US/firefox/addon/native_hls_playback)
 * using Chrome installing [Native HLS playback](https://chrome.google.com/webstore/detail/native-hls-playback/emnphkkblegpebimobpbekeedfgemhof)

There is also a small HTML page that use hls.js.

Using Docker image
===============
You can start the application using the docker image :

        docker run -p 8554:8554 -it mpromonet/v4l2rtspserver

You can expose V4L2 devices from your host using :

        docker run --device=/dev/video0 -p 8554:8554 -it mpromonet/v4l2rtspserver

The container entry point is the v4l2rtspserver application, then you can :

* get the help using :

        docker run -it mpromonet/v4l2rtspserver -h

* run the container specifying some paramters :

        docker run --device=/dev/video0 -p 8554:8554 -it mpromonet/v4l2rtspserver -u "" -H640 -W480 
