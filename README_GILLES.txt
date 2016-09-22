For MacOS:
==========

Building FFMPEG with the videotoolbox_decoder and audiotoolbox_decoder modules
------------------------------------------------------------------------------

[Change the --prefix option with a path that makes sense for your dev environment]

# mini config with debug
./configure --enable-debug --disable-optimizations --disable-everything --disable-vda --disable-videotoolbox --disable-doc --disable-htmlpages --disable-manpages --disable-podpages --disable-txtpages --disable-network --enable-ffplay --enable-ffprobe --enable-protocol=file --enable-filter=scale --enable-demuxer=m4v,mov,mp3 --enable-filter=aresample --enable-decoder=aac_audiotoolbox,mp3_audiotoolbox,h264_videotoolbox --prefix=/Users/gilles/Temp/ffmpeg

# mini config without debug
./configure --enable-shared --disable-everything --disable-vda --disable-videotoolbox --disable-doc --disable-htmlpages --disable-manpages --disable-podpages --disable-txtpages --disable-network --enable-ffplay --enable-ffprobe --enable-protocol=file --enable-filter=scale --enable-parser=mpegaudio,h264 --enable-demuxer=m4v,mov,mp3 --enable-filter=aresample --enable-decoder=aac_audiotoolbox,mp3_audiotoolbox,h264_videotoolbox --prefix=/Users/gilles/Temp/ffmpeg

# full config with debug
./configure --enable-debug --disable-optimizations --disable-vda --disable-videotoolbox --disable-decoder=aac,mp3,h264 --enable-decoder=aac_audiotoolbox,mp3_audiotoolbox,h264_videotoolbox --prefix=/Users/gilles/Temp/ffmpeg

# full config without debug
./configure --disable-vda --disable-videotoolbox --disable-decoder=aac,mp3,h264 --enable-decoder=aac_audiotoolbox,mp3_audiotoolbox,h264_videotoolbox --prefix=/Users/gilles/Temp/ffmpeg

Once configured, you can run make.
You can then run from the locally built binary (ffplay_g in debug mode, of ffplay in release mode), you don't need to run 'make install', since those are statically linked.

Building and Debugging with xcode
=================================
Project file: FfmpegAudioVideoToolboxDecoders.xcodeproj

* Build from the project either library (or both)
* Link from the command line to get a version of ffplay_g that's linked against the newly built module:
(replace path of the library build location for your system. On my setup, the build is under build/Debug for debug builds)

For Video:
gcc -Lbuild/Debug -lFfmpegVideotoolboxDecoder -Llibavcodec -Llibavdevice -Llibavfilter -Llibavformat -Llibavresample -Llibavutil -Llibpostproc -Llibswscale -Llibswresample -Wl,-dynamic,-search_paths_first -Qunused-arguments   -o ffplay_g cmdutils.o ffplay.o   -lavdevice -lavfilter -lavformat -lavcodec -lswresample -lswscale -lavutil -framework QTKit -framework Foundation -framework QuartzCore -framework CoreVideo -framework Foundation -framework AVFoundation -framework CoreMedia -liconv -Wl,-framework,CoreFoundation -Wl,-framework,Security -lSDLmain -lSDL -Wl,-framework,Cocoa -framework CoreGraphics -lm -lbz2 -lz -pthread -framework CoreServices -L/usr/local/lib -lSDLmain -lSDL -Wl,-framework,Cocoa -framework AudioToolbox -framework VideoToolbox

For Audio:
gcc -Lbuild/Debug -lFfmpegAudiotoolboxDecoder -Llibavcodec -Llibavdevice -Llibavfilter -Llibavformat -Llibavresample -Llibavutil -Llibpostproc -Llibswscale -Llibswresample -Wl,-dynamic,-search_paths_first -Qunused-arguments   -o ffplay_g cmdutils.o ffplay.o   -lavdevice -lavfilter -lavformat -lavcodec -lswresample -lswscale -lavutil -framework QTKit -framework Foundation -framework QuartzCore -framework CoreVideo -framework Foundation -framework AVFoundation -framework CoreMedia -liconv -Wl,-framework,CoreFoundation -Wl,-framework,Security -lSDLmain -lSDL -Wl,-framework,Cocoa -framework CoreGraphics -lm -lbz2 -lz -pthread -framework CoreServices -L/usr/local/lib -lSDLmain -lSDL -Wl,-framework,Cocoa -framework AudioToolbox -framework VideoToolbox

For both:
gcc -Lbuild/Debug -lFfmpegAudiotoolboxDecoder -lFfmpegVideotoolboxDecoder -Llibavcodec -Llibavdevice -Llibavfilter -Llibavformat -Llibavresample -Llibavutil -Llibpostproc -Llibswscale -Llibswresample -Wl,-dynamic,-search_paths_first -Qunused-arguments   -o ffplay_g cmdutils.o ffplay.o   -lavdevice -lavfilter -lavformat -lavcodec -lswresample -lswscale -lavutil -framework QTKit -framework Foundation -framework QuartzCore -framework CoreVideo -framework Foundation -framework AVFoundation -framework CoreMedia -liconv -Wl,-framework,CoreFoundation -Wl,-framework,Security -lSDLmain -lSDL -Wl,-framework,Cocoa -framework CoreGraphics -lm -lbz2 -lz -pthread -framework CoreServices -L/usr/local/lib -lSDLmain -lSDL -Wl,-framework,Cocoa -framework AudioToolbox -framework VideoToolbox

Now you can run and debug from XCode.
(don't forget to re-link manually after you rebuild the module)

Testing
=======

Test with an MP4 file that contains AAC Audio, H.264 Video or both. You can also test with an MP3 Audio file

Checking that you're loading the right module
=============================================

For Video, you will see an inverted square on the top-left corner of the image.
You can also run ffplay with -loglevel 56 to get a trace of the calls (you can use lower log values of course if you don't want to trace all the calls)

For Windows
===========

Media Foundation Audio & Video Decoders for FFMPEG
==================================================

Prerequisites
-------------

Visual Studio 2015 (Community Edition works fine)
msys2 : http://msys2.github.io/
SDL 1.2.15 (needed for ffplay) : http://libsdl.org/  download SDL-1.2.15-win32-x64.zip
yasm (needed to compile ffmpeg) : http://yasm.tortall.net/Download.html

Edit your environment variables to add yasm to your executable PATH

Modifications to ffmpeg/libavcodec
-----------------------------------

configure : modified to allow compilation of c++ file with Visual Studio and add the MF audio/video decoder modules, and added an option to specify the location of the SDL SDK

libavcodec/Makefile and libavcodec/allcodecs.c : modified to add the MF audio/video decoder modules

mf_audio_decoder.cpp and mf_video_decoder.cpp : MF audio/video decoder modules implementation

Building
--------

1. Setup your build environment: open a Visual Studio command shell ('Developer Command Prompt for VS2015') so that the Visual Studio command line tools are in your path.
2. From that shell, start an MSYS shell : run msys2_shell.bat (from where you installed msys, typically c:\msys64\msys2_shell.bat)
3. configure ffmpeg/libavcodec:
./configure --toolchain=msvc --optflags='-Zi -Og -Oy- ' --arch=x86 --enable-sdl --enable-yasm --enable-asm --enable-shared --disable-static --enable-debug --disable-optimizations --disable-everything --disable-dxva2 --disable-doc --disable-htmlpages --disable-manpages --disable-podpages --disable-txtpages --disable-network --enable-ffprobe --enable-ffplay --enable-protocol=file --enable-filter=scale --enable-demuxer=m4v,mov,mp3 --enable-filter=aresample --enable-decoder=aac_mf_audio,mp3_mf_audio,h264_mf_video --prefix=/c/Users/gilles/Temp/ffmpeg --sdl-dir=c:/Users/gilles/Develop/SDL-1.2.15
(change the install prefix to the location where you want to put the built binaries, and the sdl_dir value to where you downloaded the SDL SDK)
4. build and install
'make V=1 install'
This will build and install the libs and binaries to the location you have chosen in the configure step.

NOTE: if you edit the source files, you only need to rebuild with 'make install' or 'make V=1 install', you don't need to re-run the configure step.

Running
-------

1. Copy SDL.dll from the SDL SDK download to the 'bin' directory of your ffmpeg build output directory. You only need to do this once, because 'make install' from ffmpeg won't delete it.
2. run ffplay.exe from the ffmpeg build output directory.

Working Visual Studio
---------------------

 Project Files
 -------------
 FfmpegMfDecoders.sln --> solution file. Open this with Visual Studio
 FfmpegMfDecoders/FfmpegMfDecoders/FfmpegMfDecoders.vcxproj --> project file

 Debugging
----------
Edit the debug configuration to set the 'Command' setting to your built copy of ffplay (ex: C:\Users\gilles\Temp\ffmpeg\bin\ffplay.exe) and 'Command Arguments' to the file you want to play.
