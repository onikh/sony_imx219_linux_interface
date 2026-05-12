#!/bin/bash
#run once per boot to configure the camera pipeline
media-ctl -d /dev/media0 \
  --set-v4l2 '"imx219 10-0010":0[fmt:SRGGB10_1X10/640x480]'
echo " data pipeline configured"