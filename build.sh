#!/bin/bash
gcc -o ffmpeg-drm main.c -I/usr/include/drm -lavcodec -lz -lm -lpthread -lavcodec -lavformat -lavutil -lswresample -ldrm -L/usr/local/lib

