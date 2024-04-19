#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#include <iostream>
#include <arpa/inet.h>
#include <vector>
#include <signal.h>
#include <algorithm>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <iostream>
#include <map>
#include <tuple>

#define DEFAULT_PORT 20000
#define MAX_LISTEN_BACKLOG 500
#define DEFAULT_READ_BUFFER_SIZE 1024

////////////////////////////// THREAD STRUCTS ////////////////////////////////
struct heartbeat_arg // passed as arg for heartbeat_thread_func
{
    std::string storage_node_address;
    int storage_node_port;
    heartbeat_arg(std::string address, int port) : storage_node_address(address), storage_node_port(port) {}
};

void parse_args(int argc, char *argv[]);
void *frontend_thread_func(void *arg); // this thread handles frontend-backendmaster comms
void *heartbeat_thread_func(void *arg);
void add_connection(int *socket_fd);
void remove_connection(int socket_fd);
void add_tid(pthread_t tid);
void remove_tid(pthread_t tid);

std::vector<std::string> split_string(std::string str, const std::string delim);
bool read_config(const char *filepath);

////////////////////////////////  modes ////////////////////////////////
bool author_mode = false;
bool verbose_mode = false;
bool debug_mode = false;    // debug mode will print out all the err msgs not required by the hw
size_t port = DEFAULT_PORT; // this is the port number the main thread will listen on

////////////////////////////////  locks ////////////////////////////////
pthread_mutex_t server_status_map_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t connections_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t tids_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t heartbeat_tid_socket_mutex = PTHREAD_MUTEX_INITIALIZER;

////////////////////////////////  data structures for handling server shutdown ////////////////////////////////
std::vector<int *> comm_fds_vector;
std::vector<pthread_t> tids_vector;
std::map<pthread_t, int> heartbeat_tid_socket_map;
////////////////////////////////  data structure for heart beat monitoring ////////////////////////////////
std::vector<heartbeat_arg *> heartbeat_arg_ptrs; // this stores the pointers to args on the heap used by heartbeat_threads
std::vector<pthread_t> heartbeat_threads;        // this stores thread ids of the heartbeat threads
////////////////////////////////  signal flags ////////////////////////////////
volatile int shut_down_flag = 0;
volatile int sigusr1_flag = 0;
////////////////////////////////  signal handlers ////////////////////////////////
void shutdown_server(int signum);
void SIGUSR1_handler(int signum);

////////////////////////////////  backend comm variables ////////////////////////////////
std::string config_file_path;
std::map<std::string, bool> server_status_map;
std::map<int, std::vector<std::string>> tablet_storage_map;
std::vector<std::string> config_serverIDs; // server IDs (address + port) read from config file
int num_servers;                           // number of storage nodes

// std::vector<int> backend_heartbeat_sockets; // this stores the fds of sockets used by heartbeat threads

int listen_fd;

//////////////////////////////// READ/WRITE/PRINT HELPERS ////////////////////////////////
void verbose_print_helper_server(const int &socket_fd, const std::string &msg);
void verbose_print_helper_client(const int &socket_fd, const std::string &msg);
bool write_helper(int socket_fd, std::string msg);

//////////////////////////////// UTILITY HELPERS ////////////////////////////////
// std::string get_serverID(int server_index);
void init_tablet_storage_map();
int redirect(char first_char);

int main(int argc, char *argv[])
{
    //////////////////////////////// REGISTERING SIGINT HANDLER ////////////////////////////////
    // signal(SIGINT, shutdown_server);
    struct sigaction act;
    // clear the sigaction structure
    sigemptyset(&act.sa_mask);
    // set the handler function
    act.sa_handler = shutdown_server;
    // no flags
    act.sa_flags = 0;
    // set the new handler for SIGINT
    if (sigaction(SIGINT, &act, NULL) < 0)
    {
        std::cerr << "sigaction for SIGINT failed..." << std::endl;
        return 1;
    }

    //////////////////////////////// REGISTERING SIGUSR1 HANDLER ////////////////////////////////
    // signal(SIGINT, shutdown_server);
    struct sigaction act_SIGUSR1;
    // clear the sigaction structure
    sigemptyset(&act_SIGUSR1.sa_mask);
    // set the handler function
    act_SIGUSR1.sa_handler = SIGUSR1_handler;
    // no flags
    act_SIGUSR1.sa_flags = 0;
    // set the new handler for SIGINT
    if (sigaction(SIGUSR1, &act_SIGUSR1, NULL) < 0)
    {
        std::cerr << "sigaction for SIGUSR1 failed..." << std::endl;
        return 1;
    }

    // Parse command-line arguments for port number (-p), author info (-a), and verbose mode (-v)
    parse_args(argc, argv);

    if (author_mode)
    {
        // Output author info and exit
        std::cerr << "Author: CIS5050 24sp Team 13" << std::endl;
        exit(EXIT_SUCCESS);
    }

    read_config(config_file_path.c_str());

    // Setup socket and bind to port
    num_servers = config_serverIDs.size();
    std::cout << "num_servers: " << num_servers << std::endl;

    init_tablet_storage_map();

    heartbeat_arg_ptrs.resize(num_servers);
    heartbeat_threads.resize(num_servers);
    for (int i = 0; i < num_servers; i++) // create a thread for each storage node for monitoring
    {
        std::vector<std::string> temp = split_string(config_serverIDs.at(i), ":"); // split "127.0.0.1:5000" for example
        std::string server_address = temp[0];
        int server_port = std::stoi(temp[1]);
        heartbeat_arg *arg_ptr = new heartbeat_arg(server_address, server_port);
        heartbeat_arg_ptrs.push_back(arg_ptr);
        pthread_create(&heartbeat_threads.at(i), NULL, heartbeat_thread_func, arg_ptr);
    }

    // Initialize socket for listening for connections with from frontend
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (listen_fd < 0)
    {
        if (debug_mode)
        {
            perror("ERROR opening socket");
        }
        exit(EXIT_FAILURE);
    }
    // Create server address structure
    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(port);

    int opt = 1;
    int ret = setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt));

    // Bind socket to the server address
    if (bind(listen_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
    {
        if (debug_mode)
        {
            perror("ERROR on binding");
        }
        exit(EXIT_FAILURE);
    }

    // Listen for incoming connections
    if (listen(listen_fd, MAX_LISTEN_BACKLOG) < 0) // does it matter what we put in the 2nd arg?
    {
        if (debug_mode)
        {
            perror("ERROR on listen");
        }
        exit(EXIT_FAILURE);
    }

    if (debug_mode)
    {
        printf("Server is running on port %ld\n", port);
    }

    // Listen for connections
    while (!shut_down_flag)
    {

        if (debug_mode)
        {
            std::cout << "starting to loop for new connections from frontend..." << std::endl;
        }

        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        int *comm_fd = (int *)malloc(sizeof(int));

        *comm_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_addr_len);

        if (debug_mode)
        {
            std::cout << "*comm_fd: " << *comm_fd << std::endl;
        }

        add_connection(comm_fd);

        if (debug_mode)
        {
            printf("Connection from %s\n", inet_ntoa(client_addr.sin_addr));
        }

        pthread_t thread;
        int create_result = pthread_create(&thread, NULL, frontend_thread_func, comm_fd);

        if (debug_mode)
        {
            std::cout << "thread created" << std::endl;
        }
        if (debug_mode && create_result)
        {
            std::cerr << "pthread_create failed" << std::endl;
            break;
        }
        add_tid(thread);
        std::cout << "tids_vector.size(): " << tids_vector.size() << std::endl;
    }

    return 0;
}

void *frontend_thread_func(void *arg)
{
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    if (pthread_sigmask(SIG_BLOCK, &set, NULL) != 0)
    {
        std::cerr << "pthread_sigmask() failed..." << std::endl;
        return NULL;
    }

    // int socket_fd = ((thread_args *)arg)->socket_fd;
    int socket_fd = *((int *)arg);
    std::cout << "inside frontend_thread_func, socket_fd: " << socket_fd << std::endl;

    std::string buffer, command;
    ssize_t bytes_read;
    char read_buffer[DEFAULT_READ_BUFFER_SIZE]; // Temporary buffer for reading data

    //////// Send greeting message
    if (verbose_mode)
    {
        std::cerr << "[" << socket_fd << "] New connection" << std::endl;
    }
    std::string welcome_message = "+OK Server ready (Author: Team 13)\r\n";
    // bool result = write_helper(socket_fd, welcome_message);
    // if (!result)
    // {
    //     std::cout << "result = false" << std::endl;
    //     close(socket_fd);
    //     remove_connection(socket_fd);
    //     return NULL;
    // }
    write_helper(socket_fd, welcome_message);

    verbose_print_helper_server(socket_fd, welcome_message);

    //////// Start reading from the client
    while (true)
    {
        std::cout << "entering while loop here" << std::endl;
        if (shut_down_flag)
        {
            close(socket_fd);
            remove_connection(socket_fd);
            return NULL;
        }
        bytes_read = read(socket_fd, read_buffer, sizeof(read_buffer) - 1); // QUESTION: why doesn't read return 0 when SIGINT handler close the socket
        std::cout << "read in worker just unblocked" << std::endl;
        if (sigusr1_flag == 1)
        {
            std::cout << "sigusr1_flag == 1, should terminate thread" << std::endl;
            close(socket_fd);
            remove_connection(socket_fd);
            return NULL;
        }
        if (bytes_read < 0)
        {
            if (errno == EBADF)
            {
                if (verbose_mode)
                {
                    std::cout << "Socket has been closed." << std::endl;
                }
            }
            else
            {
                if (verbose_mode)
                {
                    std::cerr << "Error reading from socket: " << strerror(errno) << std::endl;
                }
            }
            break;
        }
        else if (bytes_read == 0)
        {
            if (verbose_mode)
            {
                std::cout << "Client closed connection" << std::endl;
            }
            break;
        }

        read_buffer[bytes_read] = '\0'; // Null-terminate the string
        buffer.append(read_buffer);
        verbose_print_helper_client(socket_fd, buffer);
        if (debug_mode)
        {
            std::cout << "buffer so far:" << buffer << std::endl;
        }

        if (shut_down_flag)
        {
            close(socket_fd);
            remove_connection(socket_fd);
            return NULL;
        }
        // Process all commands in the buffer
        size_t pos;
        while ((pos = buffer.find("\r\n")) != std::string::npos)
        {
            // if CRLF is found, extract token (command
            command = buffer.substr(0, pos); // Extract command
            buffer.erase(0, pos + 2);        // Remove the processed command from buffer

            if (debug_mode)
            {
                std::cout << "buffer after erase() now: " << buffer << std::endl;
            }
            // Convert the first 4 characters to lowercase to handle case-insensitive commands
            for (size_t i = 0; i < command.length() && i < 4; ++i)
            {
                command[i] = std::toupper(command[i]);
            }

            std::string response = "";

            if (command.substr(0, 4) == "INIT")
            {
                if (debug_mode)
                {
                    std::cout << "MASTER received command: " << command << std::endl;
                }

                std::vector<std::string> command_tokens = split_string(command, ","); // INIT,linhphan -> {INIT, linhphan}

                if (command_tokens.size() != 2)
                {
                    response = "-ERR Incorrect command syntax\r\n";
                    write_helper(socket_fd, response);
                    continue;
                }

                if (debug_mode)
                {
                    std::cout << "command_tokens.size(): " << command_tokens.size() << std::endl;
                }

                std::string row_key = command_tokens[1];

                if (debug_mode)
                {
                    std::cout << "row_key: " << row_key << std::endl;
                }

                char first_char = row_key.at(0);

                int tablet_no = redirect(first_char);

                if (debug_mode)
                {
                    std::cout << "tablet_no: " << tablet_no << std::endl;
                }

                if (tablet_no == -1)
                {
                    response = "-ERR invalid row key\r\n";
                }
                else
                {
                    // pthread_mutex_lock(&server_status_map_mutex);
                    // create a reference to a vector holding servers' indices storing the given tablet
                    if (debug_mode)
                    {
                        std::cout << "count: " << tablet_storage_map.count(2) << std::endl;
                    }
                    auto &tablet_servers_list = tablet_storage_map.at(tablet_no);
                    if (debug_mode)
                    {
                        std::cout << "broke here?" << std::endl;
                    }
                    std::string primary_server_ID = tablet_servers_list.at(0);
                    while (!server_status_map.at(primary_server_ID))
                    {
                        tablet_servers_list.erase(tablet_storage_map.at(tablet_no).begin());
                        tablet_servers_list.push_back(primary_server_ID);
                        primary_server_ID = tablet_servers_list.at(0);
                    }
                    response = "RDIR," + primary_server_ID + "\r\n";
                }
                write_helper(socket_fd, response);
                verbose_print_helper_server(socket_fd, response);
            }
            else if (command == "STAT")
            {
                response += "+OK,";
                pthread_mutex_lock(&server_status_map_mutex);
                for (const auto &pair : server_status_map)
                {
                    auto temp = split_string(pair.first, ":");
                    std::string serv_addr_str = temp[0];
                    std::string serv_port_str = temp[1];
                    std::string serv_status_str = std::to_string(pair.second);
                    response += serv_addr_str + ":" + serv_port_str + ":" + serv_status_str + ",";
                }
                response += "\r\n";
                write_helper(socket_fd, response);
                verbose_print_helper_server(socket_fd, response);
                pthread_mutex_unlock(&server_status_map_mutex);
            }
            else
            {
                response = "-ERR unrecognizable command\r\n";
                write_helper(socket_fd, response);
                verbose_print_helper_server(socket_fd, response);
            }
        }
    }

    close(socket_fd);
    // remove_connection(socket_fd);
    remove_connection(socket_fd);
    // std::cout << "Thread dying detached!" << std::endl;
    return NULL;
}

void add_connection(int *socket_fd)
{
    std::cout << "add_connection() starts" << std::endl;
    pthread_mutex_lock(&connections_mutex);
    comm_fds_vector.push_back(socket_fd);
    pthread_mutex_unlock(&connections_mutex);
}

void remove_connection(int socket_fd)
{
    pthread_mutex_lock(&connections_mutex);
    int index;
    for (size_t i = 0; i < comm_fds_vector.size(); i++)
    {
        if (*(comm_fds_vector.at(i)) == socket_fd)
        {
            free(comm_fds_vector.at(i));
            index = i;

            std::cout << "server is calling free() to release resources..." << std::endl;
            comm_fds_vector.erase(comm_fds_vector.begin() + i);
            break;
        }
    }
    pthread_mutex_unlock(&connections_mutex);
}

void shutdown_server(int signum)
{
    std::cout << "SIGINT received!!" << std::endl;
    shut_down_flag = 1; // sets global shutdown flag to true

    if (debug_mode)
    {
        std::cout << "comm_fds_vector.size(): " << comm_fds_vector.size() << std::endl;
        std::cout << "tids_vector.size(): " << tids_vector.size() << std::endl;
    }

    pthread_mutex_lock(&connections_mutex);
    for (int i = 0; i < comm_fds_vector.size(); i++)
    {
        std::string shut_down_message = "-ERR Server shutting down\r\n";
        write(*(comm_fds_vector.at(i)), shut_down_message.c_str(), shut_down_message.size());

        //////////////////////////////// SENDIN SIGUSR1 TO EACH CHILD ////////////////////////////////
        if (debug_mode)
        {
            std::cerr << "About to pthread_kill tids_vector.at(" << i << "): " << tids_vector.at(i) << std::endl;
        }
        std::cout << "SIGUSR1 will be sent soon..." << std::endl;
        int kill_result = pthread_kill(tids_vector.at(i), SIGUSR1);
        std::cout << "SIGUSR1 is sent..." << std::endl;
        if (debug_mode && kill_result)
        {
            std::cerr << "pthread_kill failed..." << std::endl;
        }
    }

    if (debug_mode)
    {
        std::cout << "heartbeat_threads.size(): " << heartbeat_threads.size() << std::endl;
        std::cout << "heartbeat_tid_socket_map.size(): " << heartbeat_tid_socket_map.size() << std::endl;
    }

    for (const auto &pair : heartbeat_tid_socket_map)
    {
        std::string shut_down_message = "-ERR Server shutting down\r\n";
        write_helper(pair.second, shut_down_message);

        //////////////////////////////// SENDIN SIGUSR1 TO EACH CHILD ////////////////////////////////
        if (debug_mode)
        {
            std::cerr << "About to pthread_kill tid: " << pair.first << std::endl;
            std::cout << "SIGUSR1 will be sent soon..." << std::endl;
        }

        int kill_result = pthread_kill(pair.first, SIGUSR1);
        if (debug_mode)
        {
            std::cout << "SIGUSR1 is sent..." << std::endl;
            if (kill_result)
            {
                std::cerr << "pthread_kill failed..." << std::endl;
            }
        }
    }
    pthread_mutex_unlock(&connections_mutex);

    if (debug_mode)
    {
        std::cout << "About to call pthread_join in SIGINT handler" << std::endl;
    }
    // make sure tids_vector is not modified to be empty at this point
    for (auto tid : tids_vector) // fix this first, make sure pthread_join is called
    {
        int join_result = pthread_join(tid, NULL);
        if (debug_mode && join_result)
        {
            std::cerr << "pthread_join failed" << std::endl;
        }
    }
    if (debug_mode)
    {
        std::cout << "all frontend threads joined inside SIGINT handler" << std::endl;
    }

    for (const auto& pair: heartbeat_tid_socket_map) // fix this first, make sure pthread_join is called
    {
        int join_result = pthread_join(pair.first, NULL);
        if (debug_mode && join_result)
        {
            std::cerr << "pthread_join failed" << std::endl;
        }
    }
    if (debug_mode)
    {
        std::cout << "all backend heartbeat threads joined inside SIGINT handler" << std::endl;
    }

    close(listen_fd);
    for (const auto &ptr : heartbeat_arg_ptrs)
    {
        delete ptr;
    }

    exit(EXIT_SUCCESS);
}

void verbose_print_helper_server(const int &socket_fd, const std::string &msg)
{
    if (verbose_mode)
    {
        std::cerr << "[" << socket_fd << "] S: " << msg;
    }
}

void verbose_print_helper_client(const int &socket_fd, const std::string &msg)
{
    if (verbose_mode)
    {
        std::cerr << "[" << socket_fd << "] C: " << msg;
    }
}

bool write_helper(int socket_fd, std::string msg)
{
    size_t total_bytes_to_write = msg.length();
    size_t bytes_written_so_far = 0;
    size_t bytes_left_to_write = total_bytes_to_write;
    while (bytes_written_so_far != total_bytes_to_write)
    {
        ssize_t write_result = write(socket_fd, msg.c_str() + bytes_written_so_far, bytes_left_to_write);
        if (shut_down_flag)
        {
            return false;
        }
        if (write_result < 0)
        {
            if (errno == EPIPE)
            {
                if (debug_mode)
                {
                    std::cerr << "errno == EPIPE: client might have closed the socket" << std::endl;
                }
            }
            else
            {
                if (debug_mode)
                {
                    perror("write() returns -1");
                }
            }
            return false;
        }
        else if (write_result == 0)
        {
            std::cerr << "write() returns 0, atypical behavior" << std::endl;
        }
        else
        {
            bytes_left_to_write -= write_result;
            bytes_written_so_far += write_result;
        }
    }
    return true;
}

void SIGUSR1_handler(int signum)
{
    sigusr1_flag = 1;
    std::cout << "SIGUSR1 received!" << std::endl;
}

void add_tid(pthread_t tid)
{
    pthread_mutex_lock(&tids_mutex);
    tids_vector.push_back(tid);
    pthread_mutex_unlock(&tids_mutex);
}

void remove_tid(pthread_t tid)
{
    pthread_mutex_lock(&tids_mutex);
    auto it = std::find(tids_vector.begin(), tids_vector.end(), tid);
    if (it == tids_vector.end())
    {
        std::cerr << "Error: cannot find tid in tids_vector" << std::endl;
        return;
    }
    tids_vector.erase(it);
    pthread_mutex_unlock(&tids_mutex);
}

/// @brief This helper function takes a std::string and split it based on delim and store the resulting tokens in a std::vector
/// @param str
/// @param delim
/// @return std::vector containing resulting tokens
std::vector<std::string> split_string(std::string str, const std::string delim)
{
    std::vector<std::string> tokens;
    size_t pos = 0;
    std::string token;
    while ((pos = str.find(delim)) != std::string::npos)
    {
        token = str.substr(0, pos);
        tokens.push_back(token);
        str.erase(0, pos + delim.length());
    }
    tokens.push_back(str);
    return tokens;
}

/// @brief This helper function reads the config file and populate vectors storing address and port information about all stoarge nodes
/// @param filepath
/// @return true if operations are successful, false otherwise
bool read_config(const char *filepath)
{
    // Open the file using the provided file path
    std::ifstream file(filepath);
    if (!file.is_open())
    {
        std::cerr << "Error: Could not open file " << filepath << std::endl;
        return false;
    }
    std::string line;
    while (std::getline(file, line)) // each iteration reads a line from the file
    {
        config_serverIDs.push_back(line);
    }
    file.close();
    return true;
}

void parse_args(int argc, char *argv[])
{
    int option;
    while ((option = getopt(argc, argv, "avp:d")) != -1)
    {
        switch (option)
        {
        case 'a':
            author_mode = true;
            break;
        case 'v':
            verbose_mode = true;
            break;
        case 'p':
            port = true;
            break;
        case 'd':
            debug_mode = true;
            std::cout << "setting debug_mode to true" << std::endl;
            break;
        default:
            std::cerr << "Usage: " << argv[0] << " -p <portno> (-a) (-v)\n";
            exit(EXIT_FAILURE);
        }
    }
    config_file_path = argv[optind++];
    if (debug_mode)
    {
        std::cout << "config file used is: " << config_file_path << std::endl;
    }
}

int redirect(char first_char)
{
    if ((first_char >= 'A' && first_char <= 'I') || (first_char >= 'a' && first_char <= 'i'))
    {
        return 1;
    }
    else if ((first_char >= 'J' && first_char <= 'R') || (first_char >= 'j' && first_char <= 'r'))
    {
        return 2;
    }
    else if ((first_char >= 'S' && first_char <= 'Z') || (first_char >= 's' && first_char <= 'z'))
    {
        return 3;
    }
    else
    {
        return -1;
    }
}

void *heartbeat_thread_func(void *arg)
{
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    if (pthread_sigmask(SIG_BLOCK, &set, NULL) != 0)
    {
        std::cerr << "pthread_sigmask() failed..." << std::endl;
        return NULL;
    }

    pthread_t tid = pthread_self();

    int heartbeat_interval = 1;
    int timeout_interval = heartbeat_interval * 1;

    std::string storage_node_address = ((heartbeat_arg *)arg)->storage_node_address;
    int storage_node_port = ((heartbeat_arg *)arg)->storage_node_port;
    std::string server_key = storage_node_address + ":" + std::to_string(storage_node_port);


    while (!shut_down_flag)
    {
        sleep(heartbeat_interval);
        // if (shut_down_flag)
        // {
        //     return nullptr;
        // }
        int sockfd = -1;
        struct sockaddr_in serv_addr;
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_port = htons(storage_node_port);

        if (inet_pton(AF_INET, storage_node_address.c_str(), &serv_addr.sin_addr) <= 0)
        {
            std::cerr << "Invalid address / Address not supported\n";
            return nullptr;
        }

        if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        {
            std::cerr << "Socket creation error\n";
            return nullptr;
        }

        // update the valid sockfd associated with this thread's tid 
        pthread_mutex_lock(&heartbeat_tid_socket_mutex);
        heartbeat_tid_socket_map[tid] = sockfd;
        pthread_mutex_unlock(&heartbeat_tid_socket_mutex);

        if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
        {
            server_status_map[server_key] = false; // cannot connect, assume server is dead
            if (debug_mode)
            {
                perror("Errno: ");
                std::cerr << "Unable to connect to " << server_key << std::endl;
            }
            continue;
        }

        std::string heartbeat_message = "HRBT\r\n";
        write_helper(sockfd, heartbeat_message);
        verbose_print_helper_server(sockfd, heartbeat_message);

        struct timeval tv;
        tv.tv_sec = timeout_interval;
        tv.tv_usec = 0;
        setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof tv);

        char buffer[DEFAULT_READ_BUFFER_SIZE] = {0};
        int valread = recv(sockfd, buffer, DEFAULT_READ_BUFFER_SIZE, 0);

        bool is_alive = (valread > 0);

        pthread_mutex_lock(&server_status_map_mutex);
        server_status_map[server_key] = is_alive;
        if (debug_mode)
        {
            if (is_alive)
            {
                std::cout << server_key << " is alive!" << std::endl;
            }
            else
            {
                std::cout << server_key << " is dead!" << std::endl;
            }
        }
        pthread_mutex_unlock(&server_status_map_mutex);

        if (is_alive)
        {
            std::string response = "QUIT\r\n";
            write_helper(sockfd, response);
        }
        // close(sockfd); // storage node shuts itself down if detecting this socket is closed.
    }

    return NULL;
}

void init_tablet_storage_map()
{
    for (int i = 1; i <= num_servers; i++)
    {
        int primary = i;
        int rep1 = -1;
        int rep2 = -1;

        if ((i + 1) > num_servers)
        {
            rep1 = (i + 1) % num_servers;
        }
        else
        {
            rep1 = i + 1;
        }
        if ((i + 2) > num_servers)
        {
            rep2 = (i + 2) % num_servers;
        }
        else
        {
            rep2 = i + 2;
        }
        if (debug_mode)
        {
            std::cout << "primary: " << primary << std::endl;
            std::cout << "rep1: " << rep1 << std::endl;
            std::cout << "rep2: " << rep2 << std::endl;
        }

        tablet_storage_map[i].push_back(config_serverIDs.at(primary - 1));
        tablet_storage_map[i].push_back(config_serverIDs.at(rep1 - 1));
        tablet_storage_map[i].push_back(config_serverIDs.at(rep2 - 1));
    }

    if (debug_mode)
    {
        std::cout << "tablet_storage_map.count(2): " << tablet_storage_map.count(2) << std::endl;
    }
}