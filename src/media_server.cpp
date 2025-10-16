#include <pistache/endpoint.h>
#include <pistache/router.h>
#include <pistache/http.h>
#include <iostream>
#include <thread>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <string>
#include <signal.h>
#include <sys/wait.h>
#include <atomic>
#include "shared_list.h"

using namespace Pistache;
using namespace std;

#define PORT 9090

#ifndef CLIENT
#define CLIENT "media_endpoint"
#endif

static string g_wavefile = "sample.wav";
static string g_codec;
static uint16_t g_port = PORT;
static string g_shared_mem_name;

// Global variables for thread communication
queue<Data> taskQueue;
mutex queueMutex;
condition_variable queueCV;
atomic<bool> b_running{true};

sigval value;

void sigchld_handler(int sig) {
    // terminate the exited spawned processes - avoid zombies creation
    pid_t pid;
    while ((pid = waitpid(-1, NULL, WNOHANG)) > 0) {
        cout << "Child of PID " << pid << " terminated\n";
    }
}

#define STR2CHAR(X) (char*)X.c_str()

string setenvvar(string var, const char* name) {
    char *env = getenv(name);
    if (env == NULL)
        return "";
    else
        return var + "=" + string(env);
}

pid_t launch_background(Data &data) {
    pid_t pid = fork();
    if (pid == -1) {
        perror("fork failed");
        return -1;
    } else if (pid == 0) {
        // Child process
        setsid(); // Start new session
        string str_port = to_string(data.port);
        string str_dport = to_string(data.dest_port);
        string str_dur = to_string(data.duration);
        string server = (data.client)? "" : "--server";
        char *const argv[] =
        {
            (char*)CLIENT,
            (char*)"--local-port",  STR2CHAR(str_port),
            (char*)"--remote-addr", data.dest_address,
            (char*)"--remote-port", STR2CHAR(str_dport),
            (char*)"--duration",    STR2CHAR(str_dur),
            (char*)"--wavefile",    STR2CHAR(g_wavefile),
            (char*)"--codec",       STR2CHAR(g_codec),
            (char*)"--shared-mem",  STR2CHAR(g_shared_mem_name),
            STR2CHAR(server),
            NULL
        };

        string _url = STR2CHAR( setenvvar("influx_URL", "URL") );
        string _token = setenvvar("influx_token", "token");
        string _org = setenvvar("influx_org", "org");
        string _bucket = setenvvar("influx_bucket", "bucket");

        char *const envp[] =
        {
            STR2CHAR(_url),
            STR2CHAR(_token),
            STR2CHAR(_org),
            STR2CHAR(_bucket),
            NULL
        };

        execvpe(CLIENT, argv, envp);
        perror("exec failed");
        _exit(1);
    }
    // Parent returns child PID
    return pid;
}

// Worker thread function
void workerThread(SharedList& shared_list) {
    while (b_running) {
        unique_lock<mutex> lock(queueMutex);

        // Wait for a task or shutdown signal
        queueCV.wait(lock, [] {
            return !taskQueue.empty() || !b_running;
        });

        if (!b_running) break;

        // Get the next task
        Data data = taskQueue.front();
        taskQueue.pop();
        lock.unlock();

        // Core work
        shared_list.lock();
        Data* fetched_data = shared_list.fetch_element(data.port);


        if (fetched_data) {
            // update data of existing element, but keep the original pid
            data.pid = fetched_data->pid;
            if (shared_list.update_element(&data) == ERROR)
                cout << "Failed to update: {" << shared_list.print_element(&data) << " }\n";
            else {
                // notify the client process with a real-time signal
                if (sigqueue(fetched_data->pid, SIGRTMIN, value) == -1) {
                    cout << "Non-existing process: {" << shared_list.print_element(&data) << " }\n";
                    if (shared_list.remove_element(fetched_data->port) == ERROR)
                        cout << "Failed to remove : {" << shared_list.print_element(&data) << " }\n";
                    fetched_data = NULL;
                }

            }
        }
        if (fetched_data == NULL) {
            // call new process
            pid_t pid = launch_background(data);
            if (pid != -1) {
                data.pid = pid;
                if (shared_list.add_element(&data) == ERROR)
                    cout << "Failed to add: {" << shared_list.print_element(&data) << " }\n";
            }
        }
        shared_list.unlock();

    }
    cout << "\nWorker Thread ended\n";
}

class RestAPIHandler {
public:
    void setupRoutes(Rest::Router& router) {
        using namespace Rest;

        // Add a task to the worker queue
        Routes::Get(router, "/stream", Routes::bind(&RestAPIHandler::addTask, this));

        // Get list status
        Routes::Get(router, "/status", Routes::bind(&RestAPIHandler::getStatus, this));
    }

    void addTask(const Rest::Request& request, Http::ResponseWriter response) {
        try {
            // Parse input
            Data data;
            auto query = request.query();
            data.client = (query.has("client"))? 1 : 0;
            data.port = stoi(query.get("port").value_or("0"));
            if (!data.port) {
                response.send(Http::Code::Bad_Request, "Missing port parameter");
                return;
            }
            string address = query.get("daddress").value_or("127.0.0.1");
            if (address.length() > 15) {
                response.send(Http::Code::Bad_Request, "Incorrect remote IP address");
                return;
            } else {
                strncpy(data.dest_address, address.c_str(), sizeof(data.dest_address));
            }
            data.dest_port = stoi(query.get("dport").value_or("5000"));
            if (!data.dest_port) {
                response.send(Http::Code::Bad_Request, "Missing dport parameter");
                return;
            }
            data.duration = stoi(query.get("duration").value_or("0"));
            if (data.duration < 0) {
                response.send(Http::Code::Bad_Request, "Wrong duration parameter");
                return;
            }


            // Add task to worker queue
            {
                lock_guard<mutex> lock(queueMutex);
                taskQueue.push(data);
            }
            queueCV.notify_one();

            response.send(Http::Code::Ok);
        } catch (const exception& e) {
            response.send(Http::Code::Internal_Server_Error, e.what());
        }
    }

    void getStatus(const Rest::Request&, Http::ResponseWriter response) {
        SharedList shared_list(t_client, g_shared_mem_name.c_str());
        shared_list.lock();
        string output = shared_list.print_list();
        shared_list.unlock();
        response.send(Http::Code::Ok, output);
    }

};

void cleanup(int sig)
{
    b_running = false;
    queueCV.notify_one();

}

static const char desc[] =
"                                                                   \n"
"%s [-p PORT] [-w WAFEFILE] [-h]                                    \n"
"                                                                   \n"
"where:                                                             \n"
"                                                                   \n"
"-p PORT             The server's listening port (default: %d)      \n"
"-w WAFEFILE         The wavefile clients will send                 \n"
"-c CODEC            ITU G.711 'pcma' or 'pcmu' (default: pcmu)     \n"
"--help -h           This help                                      \n"
"                                                                   \n"
;


int main(int argc, char* argv[])
{

    int opt;
    while ((opt = getopt(argc, argv, "hp:w:c:")) != -1) {
        switch (opt) {
            case 'p':
                g_port = atoi(optarg);
                break;
            case 'w':
                g_wavefile = optarg;
                break;
            case 'c':
                g_codec = optarg;
                break;
            case 'h':
            default:
                printf(desc, basename(argv[0]), PORT);
                return 0;

        }
    }

    value.sival_int = 0;  // dummy payload


    // Register termination signal
    signal(SIGINT, cleanup);

    // Set up SIGCHLD handler
    struct sigaction sa;
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    // Initialize the in-shared-memory list
    g_shared_mem_name = SHM_NAME + string("_") + to_string(g_port);
    SharedList shared_list = SharedList(t_server, g_shared_mem_name.c_str());
    shared_list.initialize();

    // Start worker thread
    thread worker(workerThread, ref(shared_list));

    // Set up REST API server
    Pistache::Address addr(Pistache::Ipv4::any(), Pistache::Port(g_port));
    auto opts = Pistache::Http::Endpoint::options().threads(2);

    RestAPIHandler handler;
    Rest::Router router;
    handler.setupRoutes(router);

    Http::Endpoint server(addr);
    server.init(opts);
    server.setHandler(router.handler());


    cout << "Server starting on port " << g_port << endl;
    server.serveThreaded();

    while(b_running) {
        pause();
    }

    worker.join();

    cout << "Server shutdown complete" << endl;
    return 0;
}
