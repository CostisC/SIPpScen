#ifndef SHARED_LIST_H
#define SHARED_LIST_H

#include <iostream>
#include <pthread.h>

#define SHM_NAME "/media_server_shm"
#define ADDR_SZ 16
#ifndef MAX_NODES
#define MAX_NODES 1000
#endif

enum rc {
    ERROR = 0,
    SUCCESS = 1
};

enum type_t {
    t_server,
    t_client,
};

typedef struct Data_t {
    int port;
    int dest_port;
    char dest_address[ADDR_SZ];
    int duration;
    pid_t pid;
    unsigned short client;    // 1 if a client
} Data;

typedef struct ListNode_t {
    Data data;
    short next;
} ListNode;

typedef struct SharedLst_t {
    pthread_mutex_t mutex;
    ListNode nodes[MAX_NODES];
    short head;
    short count;
} SharedLst;



class SharedList {
    SharedLst *list;
    short size;
    type_t type;
    char shared_mem_name[100];
    bool mutex_initialized = false;
    bool mutex_acquired = false;

public:
    // if shared_mem_name is NULL, create a local list, for testing
    SharedList(type_t type=t_client, const char* shared_mem_name=SHM_NAME);
    ~SharedList();
    void initialize();              // should only be used by server
    std::string print_list () const;
    static const char* print_element (Data *data);
    int add_element(Data *data);
    int update_element(Data *data);
    int remove_element(int port);
    Data* fetch_element(int port) const;
    void lock();
    void unlock();

};

#endif
