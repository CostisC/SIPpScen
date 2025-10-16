#include <thread>
#include <chrono>
#include <iostream>
#include <atomic>
#include <condition_variable>
#include <signal.h>
#include "shared_list.h"
#include "rtp_endpoint.h"
#include "influxdb_client.h"

using namespace std;

static const char desc[] =
"                                                                           \n"
"%s <options>                                                               \n"
"                                                                           \n"
"where <options>:                                                           \n"
"                                                                           \n"
"--server                   Server mode (default: Client)                   \n"
"--local-port=PORT          Local RTP port (default: 4000)                  \n"
"--duration=DUR             Call duration (ms)                              \n"
"                           Server default: 60s                             \n"
"--codec=CODEC              ITU G.711 'pcma' or 'pcmu' (default: pcmu)      \n"
"--shared-mem=MEM           The name of memory shared with media_server     \n"
"                           (default: %s)                                   \n"
"                                                                           \n"
" Client-only options:                                                      \n"
"--remote-addr=ADDRESS      Remote RTP address                              \n"
"--remote-port=PORT         Remote RTP port                                 \n"
"--wavefile=filename        WAVE audio file (mono 8000Hz) (default: %s)     \n"
"                                                                           \n"
"--help -h                  This help                                       \n"
"                                                                           \n"
"Built on pjsip version %s (https://github.com/pjsip/pjproject)             \n"
"                                                                           \n"
;

#define SAMPLE_WAV "sample.wav"

static Data conf;
string g_wavefile = SAMPLE_WAV;
string g_codec;
string g_shared_mem;
InfluxDBClient *g_pInfluxdb;
condition_variable cv;
mutex cv_m;
atomic<bool> b_running{true};


bool g_server = false;

void endThread() {
    lock_guard<mutex> lk(cv_m);
    b_running = false;
    cv.notify_one();
}

void endpoint_thread()
{
    pj_thread_desc thread_desc;
    pj_thread_t *pj_thread;
    pj_thread_register("endpoint_thread", thread_desc, &pj_thread);

    try
    {
        pjmedia_dir dir = (g_server)? PJMEDIA_DIR_DECODING : PJMEDIA_DIR_ENCODING;
        cout << "create endpoint: " << SharedList::print_element(&conf) << endl;
        RTP_endpoint endpoint(conf.port, 1, dir, g_codec.c_str());
        while (b_running) {
            unique_lock<mutex> lk(cv_m);
            endpoint.setRemoteAddr(conf.dest_address, conf.dest_port);
            endpoint.createStream();
            if (g_server) {
                 endpoint.startStream();
                 lk.unlock();
                 while(true) {
                    this_thread::sleep_for(chrono::seconds(10));
                    // endpoint.print_stream_stat();
                    g_pInfluxdb->send("audio", "type=TX",
                        "mos=" + to_string(endpoint.get_MOS()));
                    if (!b_running) return;
                 }

            } else {
                endpoint.startStream(g_wavefile.c_str());
                endpoint.startStreaming();
                this_thread::sleep_for(chrono::milliseconds(conf.duration));
                // endpoint.print_stream_stat();
                g_pInfluxdb->send("audio", "type=RX",
                    "mos=" + to_string(endpoint.get_MOS()));
                endpoint.stopStreaming();
            }

            // cout << "Wait\n";
            cv.wait(lk);
        }
    }
    catch (const char *e)
    {
        cout << "Failed to create endpoint: " << SharedList::print_element(&conf) << endl;
        cout << e << endl;
        _exit(1);
    }
}


int main(int argc, char *argv[])
{
    pj_getopt_option long_options[] = {
        {"local-port",          1, 0, 'p'},
        {"remote-addr",         1, 0, 'i'},
        {"remote-port",         1, 0, 'r'},
        {"duration",            1, 0, 'd'},
        {"wavefile",            1, 0, 'w'},
        {"codec",               1, 0, 'c'},
        {"server",              0, 0, 's'},
        {"shared-mem",          1, 0, 'm'},
        {"help",                0, 0, 'h'},
        { NULL, 0, 0, 0 },
    };
    conf.port = 4000;

    /* Parse arguments */
    int c, option_index;
    pj_optind = 0;

    while((c=pj_getopt_long(argc,argv, "h", long_options, &option_index))!=-1) {

        switch (c) {
        case 'p':
            conf.port = (pj_uint16_t) atoi(pj_optarg);
            if (!conf.port) {
                printf("Error: invalid local port %s\n", pj_optarg);
                return 1;
            }
            break;
        case 'r':
            conf.dest_port = (pj_uint16_t) atoi(pj_optarg);
            if (!conf.dest_port) {
                printf("Error: invalid remote port %s\n", pj_optarg);
                return 1;
            }
            break;
        case 'i':
            strncpy(conf.dest_address, pj_optarg, ADDR_SZ);
            break;
        case 'd':
            conf.duration = (pj_uint16_t) atoi(pj_optarg);
            if (!conf.duration) {
                printf("Error: invalid duration %s\n", pj_optarg);
                return 1;
            }
            break;
        case 's':
            g_server = true;
            break;

        case 'w':
            g_wavefile = pj_optarg;
            break;

        case 'c':
            g_codec = pj_optarg;
            break;

        case 'm':
            g_shared_mem = pj_optarg;
            break;

        case 'h':
            printf(desc, basename(argv[0]), SHM_NAME, SAMPLE_WAV, PJ_VERSION);
            return 0;

        default:
            printf("Invalid options %s\n", argv[pj_optind]);
            return 1;


        }
    }

    SharedList shared_list(t_client, g_shared_mem.c_str());

    // Block SIGRTMIN
    sigset_t rt_sig;
    sigemptyset(&rt_sig);
    sigaddset(&rt_sig, SIGRTMIN);
    sigprocmask(SIG_SETMASK, &rt_sig, NULL);
    siginfo_t info;
    timespec timeout;
    // TODO: configurable
    // server defaults to 60s lifetime
    if (g_server && conf.duration == 0)
        conf.duration = 60000;
    timeout.tv_sec = conf.duration * 2 / 1000;
    timeout.tv_nsec = 0;

    char *influx_URL = getenv("influx_URL");
    if (influx_URL != NULL) {
        g_pInfluxdb = new InfluxDBClient(
            string(influx_URL),
            string(getenv("influx_org")),
            string(getenv("influx_bucket")),
            string(getenv("influx_token"))
        );
    } else
        g_pInfluxdb = new InfluxDBClient();


    thread t1(endpoint_thread);

    while (true)
    {
        int sig = sigtimedwait(&rt_sig, &info, &timeout);
        if (sig == -1) {
            if (errno == EAGAIN) {
                // timed-out
                // perror("timeout");
                endThread();
                break;

            } else {
                printf("error\n");
                exit(EXIT_FAILURE);
            }
        }
        shared_list.lock();
        Data *new_conf = shared_list.fetch_element(conf.port);
        shared_list.unlock();
        {
            lock_guard<mutex> lk(cv_m);
            memcpy(&conf, new_conf, sizeof(Data));
            // cout << "new signal\n";
        }

        cv.notify_one();

    }
    t1.join();
    shared_list.lock();
    shared_list.remove_element(conf.port);
    shared_list.unlock();
    delete g_pInfluxdb;
}
