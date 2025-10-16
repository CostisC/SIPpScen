#include "shared_list.h"
#include <string.h>

int main(int argc, char *argv[])
{
    char shared_mem_name[100] = SHM_NAME;
    if (argc == 2)
        strcpy(shared_mem_name, argv[1]);

    SharedList shared_list(t_client, shared_mem_name);
    shared_list.lock();
    // // Add some nodes
    // Data dd = { 20, 200, "200.0.0.1", 20};
    // shared_list.add_element(&dd);
    std::cout << shared_list.print_list() << std::endl;
    shared_list.unlock();

    return 0;
}
