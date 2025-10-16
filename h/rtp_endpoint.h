
#ifndef _RTP_ENDPOINT
#define _RTP_ENDPOINT

#include <pjlib.h>
#include <pjlib-util.h>
#include <pjmedia.h>
#include <pjmedia-codec.h>
#include <pjmedia/transport_srtp.h>

#include <stdlib.h> /* atoi() */
#include <stdio.h>


class RTP_endpoint
{
    pj_caching_pool cp;
    pjmedia_transport *transport = NULL;
    pjmedia_endpt *med_endpt;
    pj_pool_t *pool;
    pjmedia_port *play_file_port = NULL;
    pjmedia_master_port *master_port = NULL;
    pjmedia_stream *stream = NULL;
    pjmedia_port *stream_port;
    pjmedia_stream_info info;
    pjmedia_codec_param codec_param;
    pj_uint16_t local_port;
    pj_sockaddr_in remote_addr;

    pj_status_t status;

    pj_status_t createSocket(pj_sockaddr_in* socket, const char* ip_addr, pj_uint16_t port);
    pj_status_t createMemPool(const char* name="app", pj_size_t initial=4000, pj_size_t increment=0);
    pj_status_t init_codecs(const pjmedia_codec_info** codec_info, const char* codec_id = nullptr);
    static const char *good_number(char *buf, unsigned buf_size, pj_int32_t val);
    float compute_MOS(float pkt_loss_rate, float rtt) const;

public:
    RTP_endpoint(pj_uint16_t local_port=4000, int log_level=1,
        pjmedia_dir=PJMEDIA_DIR_ENCODING, const char* codec_id = nullptr);
    ~RTP_endpoint();
    void setRemoteAddr(const char* ip_addr, pj_uint16_t port);
    void createStream();
    void startStream();
    void startStream(const char* wavefile);
    void stopStreaming();
    void startStreaming();
    void print_stream_stat() const;
    float get_MOS() const;
};


#endif