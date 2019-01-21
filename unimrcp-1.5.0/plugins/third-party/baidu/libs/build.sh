#!/bin/sh
gcc -c -fPIC ttscurl.c -I../include
gcc -c -fPIC common.c -I../include
gcc -c -fPIC token.c -I../include
rm -rf libbtts.so
gcc -shared -o libbtts.so common.o  token.o ttscurl.o
cp /usr/local/src/mrcp-tts/unimrcp-1.5.0/plugins/third-party/baidu/libs/libbtts.so /usr/local/unimrcp/lib/

