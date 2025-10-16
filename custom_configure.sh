./configure CFLAGS="-fPIC -g -O3 -DNDEBUG -msoft-float -fno-builtin "   \
	--disable-gsm-codec \
	--disable-speex-aec \
	--disable-speex-codec \
  	--disable-upnp \
  	--disable-video    \
  	--disable-small-filter  \
  	--disable-large-filter  \
  	--disable-l16-codec     \
  	--disable-g722-codec    \
  	--disable-g7221-codec   \
  	--disable-ilbc-codec    \
  	--disable-sdl           \
  	--disable-v4l2          \
  	--disable-openh264      \
  	--disable-vpx           

#-DPJMEDIA_STREAM_ENABLE_XR -DPJMEDIA_HAS_RTCP_XR
