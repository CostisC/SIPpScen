#include "rtp_endpoint.h"
#include <chrono>

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)
#define check_status(X) \
    if (X)              \
    throw __FILE__ " Error: l." TOSTRING(__LINE__) " - " #X
#define PRT(X) printf("%s\n", X)

RTP_endpoint::RTP_endpoint(pj_uint16_t local_port, int log_level,
                           pjmedia_dir dir, const char *codec_id) : local_port(local_port)
{

    pj_log_set_level(log_level);
    check_status(pj_init());
    /* Must create a pool factory before we can allocate any memory. */
    const pjmedia_codec_info *codec_info;
    // check_status(pj_init());
    check_status(createMemPool());
    // codec initialization
    check_status(init_codecs(&codec_info, codec_id));
    /* Create event manager */
    check_status(pjmedia_event_mgr_create(pool, 0, NULL));

    /* Create Stream */
    /* Reset stream info. */
    pj_bzero(&info, sizeof(info));
    /* Initialize stream info formats */
    info.type = PJMEDIA_TYPE_AUDIO;
    info.dir = dir;
    pj_memcpy(&info.fmt, codec_info, sizeof(pjmedia_codec_info));
    info.tx_pt = codec_info->pt;
    info.rx_pt = codec_info->pt;
    auto now = std::chrono::system_clock::now();
    auto epoch_time = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    pj_srand(epoch_time);
    info.ssrc = pj_rand();

    /* Create media transport */
    check_status(pjmedia_transport_udp_create(med_endpt, NULL, local_port,
                                              0, &transport));
    /* Get codec default param for info */
    check_status(pjmedia_codec_mgr_get_default_param(
        pjmedia_endpt_get_codec_mgr(med_endpt), codec_info, &codec_param));
}

RTP_endpoint::~RTP_endpoint()
{
    /* Destroy master port */
    if (master_port)
    {
        pjmedia_master_port_destroy(master_port, PJ_FALSE);
    }

    /* Destroy stream */
    if (stream)
    {
        pjmedia_transport *tp;

        tp = pjmedia_stream_get_transport(stream);
        pjmedia_stream_destroy(stream);

        pjmedia_transport_media_stop(tp);
        pjmedia_transport_close(tp);
    }

    /* Destroy file ports */
    if (play_file_port)
        pjmedia_port_destroy(play_file_port);

    /* Destroy event manager */
    pjmedia_event_mgr_destroy(NULL);

    /* Release application pool */
    pj_pool_release(pool);

    /* Destroy media endpoint. */
    pjmedia_endpt_destroy(med_endpt);

    /* Destroy pool factory */
    pj_caching_pool_destroy(&cp);

    /* Shutdown PJLIB */
    pj_shutdown();
}

pj_status_t RTP_endpoint::init_codecs(const pjmedia_codec_info **codec_info, const char *codec_id)
{
    /* Register G.711 codecs */
    status = pjmedia_codec_g711_init(med_endpt);
    if (status)
        return status;

    /* Find which codec to use. */
    if (codec_id)
    {
        unsigned count = 1;
        pj_str_t str_codec_id = pj_str(const_cast<char *>(codec_id));
        pjmedia_codec_mgr *codec_mgr = pjmedia_endpt_get_codec_mgr(med_endpt);
        status = pjmedia_codec_mgr_find_codecs_by_id(codec_mgr,
                                                     &str_codec_id, &count,
                                                     codec_info, NULL);
        if (status != PJ_SUCCESS)
        {
            printf("Error: unable to find codec %s\n", codec_id);
        }
        return status;
    }
    else
    {
        /* Default to pcmu */
        return (pjmedia_codec_mgr_get_codec_info(pjmedia_endpt_get_codec_mgr(med_endpt),
                                                 0, codec_info));
    }
}

pj_status_t RTP_endpoint::createSocket(pj_sockaddr_in *socket,
                                       const char *ip_addr, pj_uint16_t port)
{
    const pj_str_t ip = pj_str(const_cast<char *>(ip_addr));
    pj_bzero(socket, sizeof(*socket));
    return pj_sockaddr_in_init(socket, &ip, port);
}

pj_status_t RTP_endpoint::createMemPool(const char *name,
                                        pj_size_t initial,
                                        pj_size_t increment)
{
    if (!increment)
        increment = initial;

    pj_caching_pool_init(&cp, &pj_pool_factory_default_policy, 0);

    /*
     * Initialize media endpoint.
     * This will implicitly initialize PJMEDIA too.
     */
    status = pjmedia_endpt_create(&cp.factory, NULL, 1, &med_endpt);
    if (status)
        return status;

    /* Create memory pool for application purpose */
    pool = pj_pool_create(&cp.factory, /* pool factory         */
                          name,        /* pool name.           */
                          initial,     /* init size            */
                          increment,   /* increment size       */
                          NULL         /* callback on error    */
    );

    return PJ_SUCCESS;
}

void RTP_endpoint::setRemoteAddr(const char *ip_addr, pj_uint16_t port)
{
    // pj_sockaddr_in remote_addr;
    pj_bzero(&remote_addr, sizeof(remote_addr));
    check_status(createSocket(&remote_addr, ip_addr, port));
    /* Copy remote address to info*/
    pj_memcpy(&info.rem_addr, &remote_addr, sizeof(pj_sockaddr_in));

    pj_sockaddr_cp(&info.rem_rtcp, &info.rem_addr);
    check_status(pj_sockaddr_set_port(&info.rem_rtcp,
                                      pj_sockaddr_get_port(&info.rem_rtcp) + 1));
}

void RTP_endpoint::createStream()
{
    /* Now that the stream info is initialized, we can create the stream.  */
    status = pjmedia_stream_create(med_endpt, pool, &info,
                                   transport,
                                   NULL, &stream);

    if (status)
    {
        pjmedia_transport_close(transport);
        throw "Error creating stream";
    }
    /* Start media transport */
    pjmedia_transport_media_start(transport, 0, 0, 0, 0);
    /* Get the port interface of the stream */
    check_status(pjmedia_stream_get_port(stream, &stream_port));
}

void RTP_endpoint::startStream(const char *wavfile)
{
    if (wavfile && !play_file_port)
    {
        unsigned wav_ptime;

        wav_ptime = PJMEDIA_PIA_PTIME(&stream_port->info);
        check_status(pjmedia_wav_player_port_create(pool, wavfile, wav_ptime,
                                                    0, -1, &play_file_port));

        check_status(pjmedia_master_port_create(pool, play_file_port, stream_port,
                                                0, &master_port));
    }
    startStream();
}

void RTP_endpoint::startStream()
{
    // info.ssrc = pj_rand();

    /* Start streaming */
    check_status(pjmedia_stream_start(stream));

    char addr[PJ_INET_ADDRSTRLEN];
    if (info.dir == PJMEDIA_DIR_DECODING)
        printf("Stream is active, dir is recv-only, local port is %d\n",
               local_port);
    else if (info.dir == PJMEDIA_DIR_ENCODING)
        printf("Stream is active, dir is send-only, sending to %s:%d\n",
               pj_inet_ntop2(pj_AF_INET(), &remote_addr.sin_addr, addr,
                             sizeof(addr)),
               pj_ntohs(remote_addr.sin_port));
    else
        printf("Stream is active, send/recv, local port is %d, "
               "sending to %s:%d\n",
               local_port,
               pj_inet_ntop2(pj_AF_INET(), &remote_addr.sin_addr, addr,
                             sizeof(addr)),
               pj_ntohs(remote_addr.sin_port));
}

void RTP_endpoint::stopStreaming()
{
    check_status(pjmedia_master_port_stop(master_port));
}
void RTP_endpoint::startStreaming()
{

    check_status(pjmedia_master_port_start(master_port));
}

const char *RTP_endpoint::good_number(char *buf, unsigned buf_size, pj_int32_t val)
{
    if (val < 1000)
    {
        pj_ansi_snprintf(buf, buf_size, "%d", val);
    }
    else if (val < 1000000)
    {
        pj_ansi_snprintf(buf, buf_size, "%d.%dK",
                         val / 1000,
                         (val % 1000) / 100);
    }
    else
    {
        pj_ansi_snprintf(buf, buf_size, "%d.%02dM",
                         val / 1000000,
                         (val % 1000000) / 10000);
    }

    return buf;
}

/*
    MOS computaion, E-Model
    pkt_loss_rate: 0-1 percentage
    jitter: milliseconds
    rtt: RTT delay, in milliseconds

*/
float RTP_endpoint::compute_MOS(float pkt_loss_rate, float rtt) const
{
    float ld = .024 * rtt;
    if (rtt > 177.3)
        ld += 0.11 * (rtt - 177.3);

    float le = 95 * pkt_loss_rate / (pkt_loss_rate + 10);

    float R = 94.2 - ld - le;

    return 1 + 0.035 * R + R * (R - 60) * (100 - R) * 7e-6;
}

float RTP_endpoint::get_MOS() const
{
    pjmedia_port *port;
    pjmedia_rtcp_stat stat;

    pjmedia_stream_get_stat(stream, &stat);
    pjmedia_stream_get_port(stream, &port);

    float mos;

    if (info.dir == PJMEDIA_DIR_DECODING)
    {
        if (stat.rx.update_cnt == 0)
            mos = 0.0;
        else {
            float pkg_loss_rate = (stat.rx.pkt) ? stat.rx.loss / (stat.rx.pkt + stat.rx.loss) : 0.0;
            mos = compute_MOS(pkg_loss_rate, stat.rtt.mean / 1000.0);
        }
    }

    if (info.dir == PJMEDIA_DIR_ENCODING)
    {
        if (stat.tx.update_cnt == 0)
            mos = 0.0;
        else {
            float pkg_loss_rate = (stat.tx.pkt) ? stat.tx.loss / (stat.tx.pkt) : 0.0;
            mos = compute_MOS(pkg_loss_rate, stat.rtt.mean / 1000.0);
        }
    }

    pjmedia_stream_reset_stat(stream);
    return mos;

}

void RTP_endpoint::print_stream_stat() const
{
    char duration[80], last_update[80];
    char bps[16], ipbps[16], packets[16], bytes[16], ipbytes[16];
    pjmedia_port *port;
    pjmedia_rtcp_stat stat;
    pj_time_val now;

    pj_gettimeofday(&now);
    pjmedia_stream_get_stat(stream, &stat);
    pjmedia_stream_get_port(stream, &port);

    puts("Stream statistics:");

#ifdef CODEC_STATS
    /* Print duration */
    PJ_TIME_VAL_SUB(now, stat.start);
    snprintf(duration, sizeof(duration),
             " Duration: %02ld:%02ld:%02ld.%03ld",
             now.sec / 3600,
             (now.sec % 3600) / 60,
             (now.sec % 60),
             now.msec);

    printf(" Info: audio %dHz, %dms/frame, %sB/s (%sB/s +IP hdr)\n Duration: %s\n",
           PJMEDIA_PIA_SRATE(&port->info),
           PJMEDIA_PIA_PTIME(&port->info),
           good_number(bps, sizeof(bps), (codec_param.info.avg_bps + 7) / 8),
           good_number(ipbps, sizeof(ipbps), ((codec_param.info.avg_bps + 7) / 8) +
                (40 * 1000 / codec_param.setting.frm_per_pkt / codec_param.info.frm_ptime)),
           duration);
#endif

    if (info.dir == PJMEDIA_DIR_DECODING)
    {
        if (stat.rx.update_cnt == 0)
            pj_ansi_strxcpy(last_update, "never", sizeof(last_update));
        else
        {
            pj_gettimeofday(&now);
            PJ_TIME_VAL_SUB(now, stat.rx.update);
            pj_ansi_snprintf(last_update, sizeof(last_update),
                             "%02ldh:%02ldm:%02ld.%03lds ago",
                             now.sec / 3600,
                             (now.sec % 3600) / 60,
                             now.sec % 60,
                             now.msec);
        }
        float pkg_loss_rate = (stat.rx.pkt) ? stat.rx.loss / (stat.rx.pkt + stat.rx.loss) : 0.0;

        printf(" RX stat last update: %s\n"
               "    total %s packets %sB received (%sB +IP hdr)%s\n"
               "    pkt loss=%d (%3.1f%%), dup=%d (%3.1f%%), reorder=%d (%3.1f%%)%s\n"
               "          (msec)    min     avg     max     last    dev\n"
               "    loss period: %7.3f %7.3f %7.3f %7.3f %7.3f%s\n"
               "    jitter     : %7.3f %7.3f %7.3f %7.3f %7.3f%s\n"
               "    MOS: %7.3f\n",
               last_update,
               good_number(packets, sizeof(packets), stat.rx.pkt),
               good_number(bytes, sizeof(bytes), stat.rx.bytes),
               good_number(ipbytes, sizeof(ipbytes), stat.rx.bytes + stat.rx.pkt * 32),
               "",
               stat.rx.loss,
               pkg_loss_rate * 100.0,
               stat.rx.dup,
               stat.rx.dup * 100.0 / (stat.rx.pkt + stat.rx.loss),
               stat.rx.reorder,
               stat.rx.reorder * 100.0 / (stat.rx.pkt + stat.rx.loss),
               "",
               stat.rx.loss_period.min / 1000.0,
               stat.rx.loss_period.mean / 1000.0,
               stat.rx.loss_period.max / 1000.0,
               stat.rx.loss_period.last / 1000.0,
               pj_math_stat_get_stddev(&stat.rx.loss_period) / 1000.0,
               "",
               stat.rx.jitter.min / 1000.0,
               stat.rx.jitter.mean / 1000.0,
               stat.rx.jitter.max / 1000.0,
               stat.rx.jitter.last / 1000.0,
               pj_math_stat_get_stddev(&stat.rx.jitter) / 1000.0,
               "",
               compute_MOS(pkg_loss_rate, stat.rtt.mean / 1000.0));
    }

    if (info.dir == PJMEDIA_DIR_ENCODING)
    {
        if (stat.tx.update_cnt == 0)
            pj_ansi_strxcpy(last_update, "never", sizeof(last_update));
        else
        {
            pj_gettimeofday(&now);
            PJ_TIME_VAL_SUB(now, stat.tx.update);
            pj_ansi_snprintf(last_update, sizeof(last_update),
                             "%02ldh:%02ldm:%02ld.%03lds ago",
                             now.sec / 3600,
                             (now.sec % 3600) / 60,
                             now.sec % 60,
                             now.msec);
        }
        float pkg_loss_rate = (stat.tx.pkt) ? stat.tx.loss / (stat.tx.pkt) : 0.0;
        printf(" TX stat last update: %s\n"
               "    total %s packets %sB sent (%sB +IP hdr)%s\n"
               "    pkt loss=%d (%3.1f%%), dup=%d (%3.1f%%), reorder=%d (%3.1f%%)%s\n"
               "          (msec)    min     avg     max     last    dev\n"
               "    loss period: %7.3f %7.3f %7.3f %7.3f %7.3f%s\n"
               "    jitter     : %7.3f %7.3f %7.3f %7.3f %7.3f%s\n"
               "    MOS: %7.3f\n",
               last_update,
               good_number(packets, sizeof(packets), stat.tx.pkt),
               good_number(bytes, sizeof(bytes), stat.tx.bytes),
               good_number(ipbytes, sizeof(ipbytes), stat.tx.bytes + stat.tx.pkt * 32),
               "",
               stat.tx.loss,
               pkg_loss_rate * 100.0,
               stat.tx.dup,
               stat.tx.dup * 100.0 / (stat.tx.pkt),
               stat.tx.reorder,
               stat.tx.reorder * 100.0 / (stat.tx.pkt),
               "",
               stat.tx.loss_period.min / 1000.0,
               stat.tx.loss_period.mean / 1000.0,
               stat.tx.loss_period.max / 1000.0,
               stat.tx.loss_period.last / 1000.0,
               pj_math_stat_get_stddev(&stat.tx.loss_period) / 1000.0,
               "",
               stat.tx.jitter.min / 1000.0,
               stat.tx.jitter.mean / 1000.0,
               stat.tx.jitter.max / 1000.0,
               stat.tx.jitter.last / 1000.0,
               pj_math_stat_get_stddev(&stat.tx.jitter) / 1000.0,
               "",
               compute_MOS(pkg_loss_rate, stat.rtt.mean / 1000.0));
    }

    printf(" RTT delay     : %7.3f %7.3f %7.3f %7.3f %7.3f%s\n",
           stat.rtt.min / 1000.0,
           stat.rtt.mean / 1000.0,
           stat.rtt.max / 1000.0,
           stat.rtt.last / 1000.0,
           pj_math_stat_get_stddev(&stat.rtt) / 1000.0,
           "");

}
