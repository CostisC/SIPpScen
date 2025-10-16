#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdexcept>
#include "shared_list.h"

inline char* print_elmnt(Data *data)
{
    static char sz_out[200];
    snprintf(sz_out, sizeof(sz_out),
        "source port: %-8d dest port: %-8d dest addr: %-16s duration: %-8d pid: %-8d client: %-8d",
        data->port, data->dest_port, data->dest_address, data->duration, data->pid, data->client);
    return sz_out;
}

SharedList::SharedList(type_t type, const char* shared_mem_name) : type(type)
{

    if (shared_mem_name == NULL) {
        list = (SharedLst*)malloc(sizeof(SharedLst));

    } else {
        strcpy(this->shared_mem_name, shared_mem_name);
        int flags;
        switch (type)
        {
        case t_client:
            flags = O_RDWR;
            break;

        case t_server:
        default:
            flags = O_CREAT | O_RDWR;
        }

        // Create shared memory
        int fd = shm_open(shared_mem_name, flags, 0666);
        if (fd == -1) {
            throw std::runtime_error("Failed to open shared Memory " +
                std::string(shared_mem_name));
        }

        if ( ftruncate(fd, sizeof(SharedLst)) == -1 ) {
            throw std::runtime_error("Failed to truncate memory: "
                + std::string(strerror(errno)));
        }

        // Map shared memory
        list = (SharedLst*)mmap(NULL, sizeof(SharedLst), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (close(fd) == -1)
            throw std::runtime_error("Failed to close shared memory file");


    }
}
SharedList::~SharedList()
{
    if (shared_mem_name == NULL) {
        free(list);
    } else {
        // Cleanup
        if (list) {
            if (mutex_acquired)
                unlock();

            if (mutex_initialized)
                pthread_mutex_destroy(&list->mutex);
            munmap(list, sizeof(SharedLst));
        }
        if (type == t_server)
            shm_unlink(shared_mem_name);
        }
}

void SharedList::initialize()
{
    pthread_mutexattr_t mutex_attr;
    pthread_mutexattr_init(&mutex_attr);
    pthread_mutexattr_setpshared(&mutex_attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&list->mutex, &mutex_attr);
    mutex_initialized = true;

    for(ushort i = 0; i < MAX_NODES-1; i++) {
        list->nodes[i].next = i+1;
        list->nodes[i].data.port = -1;
    }
    list->nodes[MAX_NODES-1].next = -1;
    list->head = 0;
    list->count = 0;
}

void SharedList::lock()
{
    if(!pthread_mutex_lock(&list->mutex)) {
        mutex_acquired = true;
    }
}

void SharedList::unlock()
{
    if(!pthread_mutex_unlock(&list->mutex)) {
        mutex_acquired = false;
    }
}
const char* SharedList::print_element(Data *data)
{
    return print_elmnt(data);
}

std::string SharedList::print_list() const
{
    int h = list->head;
    std::string output;
    ListNode *node;
    for(int i = 0; i < list->count; i++, h=node->next) {
        node = list->nodes + h;
        output += print_elmnt(&node->data) + std::string("\n");
        if (node->next == -1) break;
    }
    return output;
}

int SharedList::update_element(Data *data)
{
    Data *d = fetch_element(data->port);
    if(d) {
        memcpy(d, data, sizeof(Data));
        return SUCCESS;
    }
    else
        return ERROR;
}

int SharedList::add_element(Data *data)
{
    if (list->count >= MAX_NODES) return ERROR;
    short h = list->head;
    ListNode *temp = list->nodes + h;
    for(int i = 0; i < list->count; i++) {
        h = temp->next;
        temp = list->nodes+h;
    }
    memcpy(&temp->data, data, sizeof(Data));
    list->count++;
    return SUCCESS;
}
int SharedList::remove_element(int port)
{
    ushort found = 0;
    short h = list->head;
    ListNode *temp = list->nodes+h, *previous;
    for(int i = 0; i < list->count; i++) {
        if (temp->data.port == port) {
            found = 1;
            break;
        }
        previous = temp;
        h = temp->next;
        temp = list->nodes+h;
    }
    if (!found) return ERROR;

    if (h == list->head) {
        list->head = temp->next;
    } else {
        previous->next = temp->next;
    }

    list->count--;
    ListNode *p = list->nodes+(list->head);
    for(int i = 0; i < list->count-1; i++) {
        p = list->nodes + p->next;
    }
    temp->next = (p->next >= 0)? p->next : -1;
    p->next = h;

    return SUCCESS;
}

Data* SharedList::fetch_element(int port) const
{
    ListNode *n;
    for(int i = 0, h = list->head; i < list->count; i++, h=n->next) {
        n = list->nodes+h;
        if(n->data.port == port)
            return &n->data;
    }
    return NULL;

}

