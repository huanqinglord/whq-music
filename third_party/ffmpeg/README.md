# FFmpeg headers and runtime

The include directory contains unmodified headers from FFmpeg 7.1.5 (tag n7.1.5).
They are used to compile the streaming audio decoder against the public FFmpeg API.

Upstream: https://ffmpeg.org/
Source: https://github.com/FFmpeg/FFmpeg/tree/n7.1.5
Release archive: https://ffmpeg.org/releases/ffmpeg-7.1.5.tar.xz
Release archive SHA-256: de668509caf9e35e3cd162473441fdb29538c6d96ed080292b3cf9e6fc5d558f

The headers retain their upstream copyright and license notices.
See COPYING.LGPLv2.1. No FFmpeg codec implementation is modified here.
The generated avconfig.h supplies target byte order and unaligned-access macros
for consumers of the headers; it does not change FFmpeg structure layouts.

At runtime WhqMusic loads Qt's shared FFmpeg libraries:
libavutil 59, libavcodec 61, libavformat 61 and libswresample 5.
The DLL ABI versions and required symbols are checked before opening audio.
CMake copies these DLLs from the selected Qt kit into the build output on Windows.
WHQ_FFMPEG_RUNTIME_DIR can specify another compatible runtime directory.

The DLL build configuration and any additional codec licenses are those of the
Qt-distributed FFmpeg build. This project does not build or statically link FFmpeg.
