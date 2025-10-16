#include <string.h>
#include "shared_list.h"


int main(int argc, char *argv[]) {
    char shared_mem_name[100] = SHM_NAME;
    if (argc == 2)
        strcpy(shared_mem_name, argv[1]);

    printf ("%d\n", MAX_NODES);
    SharedList shared_list = SharedList(t_server, shared_mem_name);
    shared_list.initialize();
    for (int i=0; i<25; i++) {
        Data d = {i,i+5, "127.127.127.127", i+100, 200+i, (unsigned short) i};
        if(! shared_list.add_element(&d))
            break;
    }

    shared_list.lock();
    shared_list.remove_element(0);
    Data dd = {100,100, "192.168.0.1", 10, 1, 1};
    shared_list.remove_element(2);
    shared_list.add_element(&dd);
    dd = { 20, 200, "127.0.0.1", 201};
    shared_list.update_element(&dd);
    shared_list.remove_element(1);
    shared_list.remove_element(5);
    std::cout << shared_list.print_list() << std::endl;
    // getchar();
    shared_list.unlock();
    Data *d = shared_list.fetch_element(20);
    if (d)
        printf("fetched: {dest: %d, duration: %d}\n", d->dest_port, d->duration);

    getchar();

    return 0;
}
