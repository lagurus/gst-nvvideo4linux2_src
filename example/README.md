<h>Simple show of motion of vectors</h>

build with:

```
mkdir build
cd build
cmake ../
```

Default is used nvoverlaysink.
If is in code set ```my_user_data.m_nRTSPStream = 1;``` then is output transfered to /dev/video5.
For showing is needed to have installed v4l2rtspserver and v4l2loopback.

```
modprobe v4l2loopback video_nr=5 maxbuffers=8
./v4l2rtspserver -F30 /dev/video5 -Q 2
mplayer rtsp://your_ip_address:8554/unicast
```

