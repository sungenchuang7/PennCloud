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
#include <unordered_set>

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
std::map<int, std::string> group_primary_map;  // this keeps track of the current primary node of each replication group <group_no, primary_serverID>
std::map<std::string, int> serverID_group_map; // given a serverID, this map tells you which replication group this server is in
std::unordered_set<std::string> pseudo_killed_nodes;
// std::unordered_set<std::string> down_servers;
// std::map<int, std::map<int, std::vector<std::string>>> tablet_storage_map;
std::vector<std::string> config_serverIDs; // server IDs (address + port) read from config file
int num_servers;                           // number of storage nodes

// std::vector<int> backend_heartbeat_sockets; // this stores the fds of sockets used by heartbeat threads

int listen_fd;

//////////////////////////////// READ/WRITE/PRINT HELPERS ////////////////////////////////
void verbose_print_helper_server(const int &socket_fd, const std::string &msg);
void verbose_print_helper_client(const int &socket_fd, const std::string &msg);
bool write_helper(int socket_fd, std::string msg);
bool create_socket_send_helper(std::string serverID, std::string message, std::string expected_server_response);
ssize_t read_until_crlf(int sockfd, char **out);

//////////////////////////////// UTILITY HELPERS ////////////////////////////////
// std::string get_serverID(int server_index);
void init_tablet_storage_map();
void init_group_primary_map();
void init_server_status_map();
void start_heartbeat_monitoring();
int get_group_no_from_char(char first_char);
void update_server_status_map_and_group_primary_map(std::string server_key);
std::vector<std::string> get_alive_servers(int group_no);
std::string get_alive_servers_string(int group_no);
bool send_PRIM(int group_no);
std::vector<std::string> get_list_of_active_secondaries(int group_no);
bool send_PRIM_to_secondary(std::string secondary_server_ID, std::string current_primary_ID);

//////////////////////////////// RECEIVED COMMAND HANDLERS ////////////////////////////////
void INIT_handler(std::string command, int socket_fd);
void STAT_handler(int socket_fd);
void GTPM_handler(std::string command, int socket_fd);
void GTGP_handler(std::string command, int socket_fd);
void RCVY_handler(std::string command, int socket_fd);
void KILL_handler(std::string command, int socket_fd);
void RVIV_handler(std::string command, int socket_fd);
void QUIT_handler(int socket_fd, bool &quit_received);

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
    std::cerr << "num_servers: " << num_servers << std::endl;

    init_tablet_storage_map();
    init_group_primary_map();
    init_server_status_map();

    start_heartbeat_monitoring();

    // Initialize socket for listening for connections with from frontend
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (listen_fd < 0)
    {
        perror("ERROR opening socket");
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
        perror("ERROR on binding");
        exit(EXIT_FAILURE);
    }

    // Listen for incoming connections
    if (listen(listen_fd, MAX_LISTEN_BACKLOG) < 0) // does it matter what we put in the 2nd arg?
    {
        perror("ERROR on listen");
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
            std::cerr << "starting to loop for new connections from frontend..." << std::endl;
        }

        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        int *comm_fd = (int *)malloc(sizeof(int));

        *comm_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_addr_len);

        if (debug_mode)
        {
            std::cerr << "*comm_fd: " << *comm_fd << std::endl;
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
            std::cerr << "thread created" << std::endl;
        }
        if (debug_mode && create_result)
        {
            std::cerr << "pthread_create failed" << std::endl;
            break;
        }
        add_tid(thread);
        std::cerr << "tids_vector.size(): " << tids_vector.size() << std::endl;
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
    std::cerr << "inside frontend_thread_func, socket_fd: " << socket_fd << std::endl;

    std::string buffer, command;
    ssize_t bytes_read;
    char read_buffer[DEFAULT_READ_BUFFER_SIZE]; // Temporary buffer for reading data

    //////// Send greeting message
    if (verbose_mode)
    {
        std::cerr << "[" << socket_fd << "] New connection" << std::endl;
    }
    std::string welcome_message = "+OK Server ready (Author: Team 13)\r\n";

    if (!write_helper(socket_fd, welcome_message))
    {
        std::cerr << "Failed to send welcome message to client" << std::endl;
        close(socket_fd);
        remove_connection(socket_fd);
        return NULL;
    }
    verbose_print_helper_server(socket_fd, welcome_message);

    bool quit_received = false;

    //////// Start reading from the client
    while (!quit_received)
    {
        std::cerr << "entering while loop here" << std::endl;
        if (shut_down_flag)
        {
            close(socket_fd);
            remove_connection(socket_fd);
            return NULL;
        }
        bytes_read = read(socket_fd, read_buffer, sizeof(read_buffer) - 1); // QUESTION: why doesn't read return 0 when SIGINT handler close the socket
        std::cerr << "read in worker just unblocked" << std::endl;
        if (sigusr1_flag == 1)
        {
            std::cerr << "sigusr1_flag == 1, should terminate thread" << std::endl;
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
                    std::cerr << "Socket has been closed." << std::endl;
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
                std::cerr << "Client closed connection" << std::endl;
            }
            break;
        }

        read_buffer[bytes_read] = '\0'; // Null-terminate the string
        buffer.append(read_buffer);
        verbose_print_helper_client(socket_fd, buffer);
        if (debug_mode)
        {
            std::cerr << "buffer so far:" << buffer << std::endl;
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
                std::cerr << "buffer after erase() now: " << buffer << std::endl;
            }
            // Convert the first 4 characters to lowercase to handle case-insensitive commands
            for (size_t i = 0; i < command.length() && i < 4; ++i)
            {
                command[i] = std::toupper(command[i]);
            }

            std::string response = "";

            if (command.substr(0, 4) == "INIT")
            {
                INIT_handler(command, socket_fd);
            }
            else if (command == "STAT")
            {
                STAT_handler(socket_fd);
            }
            else if (command.substr(0, 4) == "GTPM")
            {
                GTPM_handler(command, socket_fd);
            }
            else if (command.substr(0, 4) == "GTGP")
            {
                GTGP_handler(command, socket_fd);
            }
            else if (command.substr(0, 4) == "RCVY")
            {
                RCVY_handler(command, socket_fd);
            }
            else if (command.substr(0, 4) == "KILL")
            {
                KILL_handler(command, socket_fd);
            }
            else if (command.substr(0, 4) == "RVIV")
            {
                RVIV_handler(command, socket_fd);
            }
            else if (command.substr(0, 4) == "QUIT")
            {
                QUIT_handler(socket_fd, quit_received);
                break;
            }
            else
            {
                std::string response = "-ERR unrecognizable command\r\n";
                write_helper(socket_fd, response);
                verbose_print_helper_server(socket_fd, response);
            }
        }
    }

    close(socket_fd);
    remove_connection(socket_fd);
    // std::cerr << "Thread dying detached!" << std::endl;
    return NULL;
}

void add_connection(int *socket_fd)
{
    std::cerr << "add_connection() starts" << std::endl;
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
            comm_fds_vector.at(i) = nullptr; // see if it works
            index = i;

            std::cerr << "server is calling free() to release resources..." << std::endl;
            comm_fds_vector.erase(comm_fds_vector.begin() + i);
            break;
        }
    }
    pthread_mutex_unlock(&connections_mutex);
}

void shutdown_server(int signum)
{
    std::cerr << "SIGINT received!!" << std::endl;
    shut_down_flag = 1; // sets global shutdown flag to true

    if (debug_mode)
    {
        std::cerr << "comm_fds_vector.size(): " << comm_fds_vector.size() << std::endl;
        std::cerr << "tids_vector.size(): " << tids_vector.size() << std::endl;
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
        std::cerr << "SIGUSR1 will be sent soon..." << std::endl;
        int kill_result = pthread_kill(tids_vector.at(i), SIGUSR1);
        std::cerr << "SIGUSR1 is sent..." << std::endl;
        if (debug_mode && kill_result)
        {
            std::cerr << "pthread_kill failed..." << std::endl;
        }
    }

    if (debug_mode)
    {
        std::cerr << "heartbeat_threads.size(): " << heartbeat_threads.size() << std::endl;
        std::cerr << "heartbeat_tid_socket_map.size(): " << heartbeat_tid_socket_map.size() << std::endl;
    }

    for (const auto &pair : heartbeat_tid_socket_map)
    {
        std::string shut_down_message = "-ERR Server shutting down\r\n";
        write_helper(pair.second, shut_down_message);

        //////////////////////////////// SENDIN SIGUSR1 TO EACH CHILD ////////////////////////////////
        if (debug_mode)
        {
            std::cerr << "About to pthread_kill tid: " << pair.first << std::endl;
            std::cerr << "SIGUSR1 will be sent soon..." << std::endl;
        }

        int kill_result = pthread_kill(pair.first, SIGUSR1);
        if (debug_mode)
        {
            std::cerr << "SIGUSR1 is sent..." << std::endl;
            if (kill_result)
            {
                std::cerr << "pthread_kill failed..." << std::endl;
            }
        }
    }
    pthread_mutex_unlock(&connections_mutex);

    if (debug_mode)
    {
        std::cerr << "About to call pthread_join in SIGINT handler" << std::endl;
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
        std::cerr << "all frontend threads joined inside SIGINT handler" << std::endl;
    }

    for (const auto &pair : heartbeat_tid_socket_map) // fix this first, make sure pthread_join is called
    {
        int join_result = pthread_join(pair.first, NULL);
        if (debug_mode && join_result)
        {
            std::cerr << "pthread_join failed" << std::endl;
        }
    }
    if (debug_mode)
    {
        std::cerr << "all backend heartbeat threads joined inside SIGINT handler" << std::endl;
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
        std::cerr << "(write_helper) trying to write: " << msg << std::endl;
        ssize_t write_result = write(socket_fd, msg.c_str() + bytes_written_so_far, bytes_left_to_write);
        std::cerr << "(write_helper) written!" << std::endl;
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
    std::cerr << "SIGUSR1 received!" << std::endl;
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
    int line_count = 0;
    while (std::getline(file, line)) // each iteration reads a line from the file
    {
        line_count++;
        if (line_count == 1)
        { // first line is the master's own addr and port, should be ignored
            continue;
        }
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
            port = std::stoi(optarg);
            break;
        case 'd':
            debug_mode = true;
            std::cerr << "setting debug_mode to true" << std::endl;
            break;
        default:
            std::cerr << "Usage: " << argv[0] << " -p <portno> (-a) (-v)\n";
            exit(EXIT_FAILURE);
        }
    }
    config_file_path = argv[optind++];
    if (debug_mode)
    {
        std::cerr << "config file used is: " << config_file_path << std::endl;
    }
}

/// @brief This function parses the first char of a row-key to determine which replication group the row belongs to
/// @param first_char
/// @return replication group number, or -1 if the input char isn't valid
int get_group_no_from_char(char first_char)
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
        std::cerr << "(heartbeat) pthread_sigmask() failed..." << std::endl;
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

        int sockfd = -1;
        struct sockaddr_in serv_addr;
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_port = htons(storage_node_port);

        if (inet_pton(AF_INET, storage_node_address.c_str(), &serv_addr.sin_addr) <= 0)
        {
            std::cerr << "(heartbeat) Invalid address / Address not supported\n";
            return nullptr;
        }

        if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        {
            std::cerr << "(heartbeat) Socket creation error\n";
            return nullptr;
        }

        // std::cerr << "SOCKFD:" << sockfd << std::endl;

        // update the valid sockfd associated with this thread's tid
        pthread_mutex_lock(&heartbeat_tid_socket_mutex);
        heartbeat_tid_socket_map[tid] = sockfd;
        pthread_mutex_unlock(&heartbeat_tid_socket_mutex);

        if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
        {
            update_server_status_map_and_group_primary_map(server_key);
            if (debug_mode)
            {
                perror("(heartbeat) Errno: ");
                std::cerr << "(heartbeat) Unable to connect to " << server_key << std::endl;
            }
            close(sockfd);
            continue;
        }

        // std::string heartbeat_message = "HRBT\r\n";
        // write_helper(sockfd, heartbeat_message);
        // verbose_print_helper_server(sockfd, heartbeat_message);

        struct timeval tv;
        tv.tv_sec = timeout_interval;
        tv.tv_usec = 0;
        setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof tv);

        // char buffer[DEFAULT_READ_BUFFER_SIZE] = {0};
        // int bytes_received = recv(sockfd, buffer, DEFAULT_READ_BUFFER_SIZE, 0);

        char *welcome_message_buffer;
        ssize_t result = read_until_crlf(sockfd, &welcome_message_buffer); // read the welcome message from the target server
        std::string actual_welcome_message{welcome_message_buffer};
        std::string expected_welcome_message = "+OK Server ready\r\n";
        if (actual_welcome_message != expected_welcome_message)
        {
            std::cerr << "(heartbeat) Warning: actual_welcome_message != expected_welcome_message" << std::endl;
        }

        bool is_alive = (result > 0);

        if (!is_alive) // if it's down
        {
            update_server_status_map_and_group_primary_map(server_key);
        }
        else
        {
            // send back QUIT to storage server so connection can be closed properly
            std::string response = "QUIT\r\n";
            write_helper(sockfd, response);

            char *quit_response_buffer;
            ssize_t result = read_until_crlf(sockfd, &quit_response_buffer);
            std::string actual_quit_response{quit_response_buffer};
            std::string expected_quit_response = "+OK Goodbye!\r\n";

            if (debug_mode)
            {
                if (result > 0)
                {
                    if (actual_quit_response != expected_quit_response)
                    {
                        std::cerr << "(heartbeat) Warning: actual_quit_response != expected_quit_response" << std::endl;
                    }
                    std::cerr << "(heartbeat) actual_quit_response: " << actual_quit_response << std::endl;
                }
                else if (result == 0)
                {
                    printf("(heartbeat) Connection closed by peer\n");
                }
                else
                {
                    printf("(heartbeat) Error reading from socket\n");
                }
            }
            // free(quit_response);
        }
        if (debug_mode)
        {
            if (is_alive)
            {
                if (pseudo_killed_nodes.count(server_key) > 0)
                {
                    std::cerr << "(heartbeat) " << server_key << " is pseudo-killed!" << std::endl;
                }
                else
                {
                    std::cerr << "(heartbeat) " << server_key << " is alive!" << std::endl;
                }
            }
            else
            {
                std::cerr << "(heartbeat) " << server_key << " is really dead!" << std::endl;
            }
        }

        close(sockfd); // storage node shuts itself down if detecting this socket is closed.
    }

    return NULL;
}

void init_tablet_storage_map()
{
    int group = -1;
    for (int i = 1; i <= num_servers; i++)
    {
        if (1 <= i && i <= 3)
        {
            group = 1;
        }
        else if (4 <= i && i <= 6)
        {
            group = 2;
        }
        else
        {
            group = 3;
        }
        // The server index in our protocol is 1-indexed, but std::vector is 0-indexed
        std::string cur_serverID = config_serverIDs.at(i - 1);

        tablet_storage_map[group].push_back(cur_serverID);
        serverID_group_map[cur_serverID] = group;
    }
}

void init_group_primary_map()
{
    for (const auto &kv_pair : tablet_storage_map)
    {
        int group_no = kv_pair.first;
        // by default in oru protocol, the first server (smallest port no) in a replication group is the primary
        group_primary_map[group_no] = kv_pair.second.at(0);
    }
}

void start_heartbeat_monitoring()
{
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
}

// Helper function to read data from socket until "\r\n" is found
ssize_t read_until_crlf(int sockfd, char **out)
{
    char *buffer = (char *)malloc(DEFAULT_READ_BUFFER_SIZE);
    if (!buffer)
    {
        perror("Failed to allocate buffer");
        return -1;
    }

    char *ptr = buffer;
    ssize_t total_read = 0;
    ssize_t n_read;

    while (1)
    {
        char c;
        n_read = recv(sockfd, &c, 1, 0);
        if (n_read < 0)
        {
            perror("Failed to read from socket");
            free(buffer);
            return -1;
        }
        else if (n_read == 0)
        {
            // Socket closed
            if (total_read == 0)
            {
                free(buffer);
                return 0;
            }
            break;
        }

        *ptr++ = c;
        total_read += n_read;

        // Check if the last two characters are "\r\n"
        if (total_read > 1 && *(ptr - 2) == '\r' && *(ptr - 1) == '\n')
        {
            break;
        }

        // Reallocate buffer if needed
        if (total_read >= DEFAULT_READ_BUFFER_SIZE - 1)
        {
            ssize_t current_size = ptr - buffer;
            char *new_buffer = (char *)realloc(buffer, current_size + DEFAULT_READ_BUFFER_SIZE);
            if (!new_buffer)
            {
                perror("Failed to reallocate buffer");
                free(buffer);
                return -1;
            }
            buffer = new_buffer;
            ptr = buffer + current_size;
        }
    }

    // Null-terminate the string
    *ptr = '\0';
    *out = buffer;
    return total_read;
}

/// @brief Initalize the status of each storage node to be alive
void init_server_status_map()
{
    for (const std::string &serverID : config_serverIDs)
    {
        server_status_map[serverID] = true;
    }
}

/// @brief This function marks the specified server's status as "down" if it's still marked as "alive". If the server's status is already "down", then this function doesn't do anything. This function is thread-safe.
/// @param server_key serverID whose status might be updated
void update_server_status_map_and_group_primary_map(std::string server_key)
{
    pthread_mutex_lock(&server_status_map_mutex);
    // cannot connect, assume server is dead
    if (!server_status_map.at(server_key))
    { // if its status is already marked as down
        pthread_mutex_unlock(&server_status_map_mutex);
        return; // don't have to mark it as down again
    }
    std::cerr << server_key << " is down!" << std::endl;
    server_status_map.at(server_key) = false;         // update the status of the server to be dead
    int group_no = serverID_group_map.at(server_key); // get the replication group number of the server
    int iteration_counter = 0;
    if (group_primary_map.at(group_no) == server_key) // if the detected down node is the primary of the group
    {
        std::cerr << server_key << " is actually the primary!" << std::endl;
        // pick the front node in the group server list as potential new primary
        std::string candidate_primary = tablet_storage_map.at(group_no).front();
        // while this node is also down
        while (!server_status_map.at(candidate_primary)) // set a time out here using iteration counter
        {
            // pop the front node from the list
            tablet_storage_map.at(group_no).erase(tablet_storage_map.at(group_no).begin());
            // push it back
            tablet_storage_map.at(group_no).push_back(candidate_primary);
            // pick the new front node as potential new primary
            candidate_primary = tablet_storage_map.at(group_no).front();
            iteration_counter++;
            if (iteration_counter >= 3)
            {
                group_primary_map.at(group_no) = "all_dead";
                pthread_mutex_unlock(&server_status_map_mutex);
                std::cerr << "SET ALL DEAD" << std::endl;
                return; // if no servers in the group are alive, don't send PRIM
            }
        }
        group_primary_map.at(group_no) = candidate_primary;
        std::cerr << "new_primary: " << candidate_primary << std::endl;
        if (!send_PRIM(group_no))
        {
            std::cerr << "send_PRIM failed... " << std::endl;
        }
        // get active secondary node list
        std::vector<std::string> list_of_active_secondaries = get_list_of_active_secondaries(group_no);
        for (std::string secondaryID : list_of_active_secondaries) {
            if (!send_PRIM_to_secondary(secondaryID, group_primary_map.at(group_no))){
                std::cerr << "Error sending PRIM to secondary at: " << secondaryID << std::endl;
            }
        }
        
    }
    else
    {
        if (!send_PRIM(group_no))
        {
            std::cerr << "Error sending PRIM to primary of group no: " << group_no << std::endl;
        }
    }

    // create_socket_send_helper(group_primary_map.at(group_no), )
    pthread_mutex_unlock(&server_status_map_mutex);
}

/// @brief This helper creates a TCP socket, sends the msg to the specified server, compare the actual response from the server with the expected response, and closes the created socket
/// @param serverID
/// @param msg
/// @param expected_server_response
/// @return true if receiving expected response from the receiving server, false if any system call fails or there's network partitions
bool create_socket_send_helper(std::string serverID, std::string message, std::string expected_server_response)
{
    std::vector<std::string> temp = split_string(serverID, ":");
    std::string storage_node_address = temp[0];
    int storage_node_port = std::stoi(temp[1]);
    int sockfd = -1;
    struct sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(storage_node_port);

    if (inet_pton(AF_INET, storage_node_address.c_str(), &serv_addr.sin_addr) <= 0)
    {
        std::cerr << "(cssh) Invalid address / Address not supported\n";
        return false;
    }

    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
    {
        std::cerr << "(cssh) Socket creation error\n";
        return false;
    }

    if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
    {
        std::cerr << "(cssh) Unable to connect to serverID" << std::endl;
        return false;
    }

    char *welcome_message_buffer;
    read_until_crlf(sockfd, &welcome_message_buffer); // read the welcome message from the target server
    std::string actual_welcome_message{welcome_message_buffer};
    std::string expected_welcome_message = "+OK Server ready\r\n";

    std::cerr << "(cssh) actual_welcome_message: " << actual_welcome_message << std::endl;

    if (actual_welcome_message != expected_welcome_message)
    {
        std::cerr << "(cssh) possible network partition" << std::endl;
        return false;
    }

    if (!write_helper(sockfd, message))
    {
        std::cerr << "(cssh) unable to write to destination server" << std::endl;
        return false;
    }

    char *buffer;
    read_until_crlf(sockfd, &buffer);
    std::string actual_server_response{buffer};
    std::cerr << "(cssh) actual_server_response: " << actual_server_response << std::endl;
    close(sockfd);
    if (expected_server_response == actual_server_response)
    {
        return true;
    }
    else
    {
        std::cerr << "(cssh) server response not expected" << std::endl;
        return false;
    }
}

/// @brief Given a replication group number, return a vector of serverIDs of servers currently still alive
/// @param group_no
/// @return a vector of serverIDs still alive
std::vector<std::string> get_alive_servers(int group_no)
{
    std::vector<std::string> res;
    std::vector<std::string> server_list = tablet_storage_map.at(group_no);
    for (const std::string &serverID : server_list)
    {
        if (server_status_map.at(serverID))
        { // if it's alive
            res.push_back(serverID);
        }
    }
    return res;
}

/// @brief Given a replication group number, return a string listing currently alive servers separated by a comma.
/// @param group_no
/// @return a string listing currently alive servers separated by a comma. ("serverID1,serverID2,serverID3")
std::string get_alive_servers_string(int group_no)
{
    std::vector<std::string> alive_servers = get_alive_servers(group_no);
    std::string res;
    for (const std::string &serverID : alive_servers)
    {
        res += serverID + ",";
    }
    res = res.substr(0, res.length() - 1);
    return res;
}

/// @brief Given a group_no, send "PRIM:<list of active nodes>" to the current primary node of that group
/// @param group_no
/// @return true if operation is successful, false otherwise
bool send_PRIM(int group_no)
{
    // get the latest list of active nodes in a group
    std::string alive_servers_string = get_alive_servers_string(group_no);
    std::string PRIM_message = "PRIM:1," + alive_servers_string + "\r\n";
    std::string expected_response = "+OK Primary updated\r\n";

    std::cerr << "send_PRIM here?" << std::endl;

    return create_socket_send_helper(group_primary_map.at(group_no), PRIM_message, expected_response);
}

void INIT_handler(std::string command, int socket_fd)
{
    std::string response = "";
    std::vector<std::string> command_tokens = split_string(command, ","); // INIT,linhphan -> {INIT, linhphan}
    if (command_tokens.size() != 2)
    {
        response = "-ERR Incorrect command syntax\r\n";
        write_helper(socket_fd, response);
        verbose_print_helper_server(socket_fd, response);
        return;
    }
    std::string row_key = command_tokens[1];
    char first_char = row_key.at(0);

    int group_no = get_group_no_from_char(first_char);

    if (group_no == -1)
    {
        response = "-ERR invalid row key\r\n";
    }
    else
    {
        std::cerr << "did we reach1" << std::endl;
        std::cerr << "group_no: " << group_no << std::endl;
        // pthread_mutex_lock(&server_status_map_mutex);
        if (group_primary_map.at(group_no) == "all_dead")
        {
            std::cerr << "did we reach2" << std::endl;
            response = "-ERR all servers are down at the moment\r\n";
        }
        else
        {
            std::cerr << "did we reach3" << std::endl;
            std::cerr << "group_primary_map.at(group_no): " << group_primary_map.at(group_no) << std::endl;

            std::vector<std::string> &tablet_servers_list = tablet_storage_map.at(group_no);
            std::string server_ID = tablet_servers_list.front();
            while (!server_status_map.at(server_ID)) // if this server is dead
            {
                tablet_servers_list.erase(tablet_storage_map.at(group_no).begin());
                tablet_servers_list.push_back(server_ID);
                server_ID = tablet_servers_list.front();
            }
            response = "RDIR," + server_ID + "\r\n";
            // update front of vector
            tablet_servers_list.erase(tablet_storage_map.at(group_no).begin());
            tablet_servers_list.push_back(server_ID);
        }
    }
    // pthread_mutex_unlock(&server_status_map_mutex);
    write_helper(socket_fd, response);
    verbose_print_helper_server(socket_fd, response);
}

void STAT_handler(int socket_fd)
{
    std::string response = "";
    response += "+OK,";
    // pthread_mutex_lock(&server_status_map_mutex);
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
    // pthread_mutex_unlock(&server_status_map_mutex);
}

void GTPM_handler(std::string command, int socket_fd)
{
    std::string response = "";
    std::cerr << "here1" << std::endl;
    std::vector<std::string> command_tokens = split_string(command, ","); // INIT,linhphan -> {INIT, linhphan}
    if (command_tokens.size() != 2)
    {
        response = "-ERR Incorrect command syntax\r\n";
        write_helper(socket_fd, response);
        verbose_print_helper_server(socket_fd, response);
        return;
    }
    std::string serverID_to_lookup = command_tokens.at(1);
    if (serverID_group_map.count(serverID_to_lookup) == 0)
    {
        response = "-ERR Invalid server address\r\n";
        write_helper(socket_fd, response);
        verbose_print_helper_server(socket_fd, response);
        return;
    }
    std::cerr << "here2" << std::endl;
    int server_group_no = serverID_group_map.at(serverID_to_lookup);
    ////
    if (group_primary_map.at(server_group_no) == "all_dead")
    {
        std::cerr << "did we reach2" << std::endl;
        response = "-ERR all servers are down at the moment\r\n";
    }
    else
    {
        std::string primaryID = group_primary_map.at(server_group_no);
        response += "+OK " + primaryID + "\r\n";
    }
    ////

    write_helper(socket_fd, response);
    verbose_print_helper_server(socket_fd, response);
}

void GTGP_handler(std::string command, int socket_fd)
{
    std::string response = "";
    std::vector<std::string> command_tokens = split_string(command, ","); // INIT,linhphan -> {INIT, linhphan}
    if (command_tokens.size() != 2)
    {
        response = "-ERR Incorrect command syntax\r\n";
        write_helper(socket_fd, response);
        verbose_print_helper_server(socket_fd, response);
        return;
    }
    std::string serverID_to_lookup = command_tokens.at(1);
    if (serverID_group_map.count(serverID_to_lookup) == 0)
    {
        response = "-ERR Invalid server address\r\n";
        write_helper(socket_fd, response);
        verbose_print_helper_server(socket_fd, response);
        return;
    }

    response += "+OK ";
    int server_group_no = serverID_group_map.at(serverID_to_lookup);
    std::vector<std::string> server_list = tablet_storage_map.at(server_group_no);
    for (const std::string &serverID : server_list)
    {
        if (server_status_map.at(serverID))
        { // if it's alive
            response += serverID + ",";
        }
    }
    response = response.substr(0, response.size() - 1) + "\r\n"; // remove last char (extra ,)
    write_helper(socket_fd, response);
    verbose_print_helper_server(socket_fd, response);
}

void RCVY_handler(std::string command, int socket_fd)
{
    std::string response = "";
    std::vector<std::string> command_tokens = split_string(command, ","); // INIT,linhphan -> {INIT, linhphan}
    if (command_tokens.size() != 2)
    {
        response = "-ERR Incorrect command syntax\r\n";
        write_helper(socket_fd, response);
        verbose_print_helper_server(socket_fd, response);
        return;
    }
    std::string serverID_to_mark_as_alive = command_tokens.at(1);
    if (serverID_group_map.count(serverID_to_mark_as_alive) == 0)
    {
        response = "-ERR Invalid server address\r\n";
        write_helper(socket_fd, response);
        verbose_print_helper_server(socket_fd, response);
        return;
    }
    // down_servers.erase(command_tokens.at(1));
    pthread_mutex_lock(&server_status_map_mutex);
    server_status_map.at(command_tokens.at(1)) = true;
    int group_no = serverID_group_map.at(serverID_to_mark_as_alive);
    if (group_primary_map.at(group_no) == "all_dead")
    { // if all other nodes in the group are dead, the revived node becomes the primary
        group_primary_map.at(group_no) = serverID_to_mark_as_alive;
    }
    auto it = pseudo_killed_nodes.find(serverID_to_mark_as_alive);
    if (it != pseudo_killed_nodes.end())
    {
        pseudo_killed_nodes.erase(it);
        std::cerr << serverID_to_mark_as_alive << " has been removed from the set." << std::endl;
    }
    else
    {
        std::cerr << serverID_to_mark_as_alive << " is not found in the set." << std::endl;
    }

    pthread_mutex_unlock(&server_status_map_mutex);
    response = "+OK node marked as alive\r\n";
    write_helper(socket_fd, response);
    verbose_print_helper_server(socket_fd, response);
    send_PRIM(group_no);
}

void KILL_handler(std::string command, int socket_fd)
{
    std::string response = "";
    std::vector<std::string> command_tokens = split_string(command, ","); // INIT,linhphan -> {INIT, linhphan}
    if (command_tokens.size() != 2)
    {
        response = "-ERR Incorrect command syntax\r\n";
        write_helper(socket_fd, response);
        verbose_print_helper_server(socket_fd, response);
        return;
    }
    std::string serverID_to_shutdown = command_tokens.at(1);
    if (serverID_group_map.count(serverID_to_shutdown) == 0)
    {
        response = "-ERR Invalid server address\r\n";
        write_helper(socket_fd, response);
        verbose_print_helper_server(socket_fd, response);
        return;
    }
    std::string message_to_send = "STDN\r\n";
    std::string expected_response = "+OK shutting down\r\n";
    if (create_socket_send_helper(serverID_to_shutdown, message_to_send, expected_response))
    {
        pseudo_killed_nodes.insert(serverID_to_shutdown);
        std::cerr << "(KILL handler) +OK shutting down received" << std::endl;
        response = "+OK storage server shutdown requested\r\n";
        std::cerr << "KILL: response: " << response << std::endl;
        write_helper(socket_fd, response);
        verbose_print_helper_server(socket_fd, response);
        update_server_status_map_and_group_primary_map(serverID_to_shutdown);
    }
    else
    {
        response = "-ERR unable to request storage server shutdown\r\n";
        std::cerr << "KILL: response: " << response << std::endl;
        write_helper(socket_fd, response);
        verbose_print_helper_server(socket_fd, response);
    }
}

void RVIV_handler(std::string command, int socket_fd)
{
    std::string response = "";
    std::vector<std::string> command_tokens = split_string(command, ","); // INIT,linhphan -> {INIT, linhphan}
    if (command_tokens.size() != 2)
    {
        response = "-ERR Incorrect command syntax\r\n";
        write_helper(socket_fd, response);
        verbose_print_helper_server(socket_fd, response);
        return;
    }
    std::string serverID_to_revive = command_tokens.at(1);
    if (serverID_group_map.count(serverID_to_revive) == 0)
    {
        response = "-ERR Invalid server address\r\n";
        write_helper(socket_fd, response);
        verbose_print_helper_server(socket_fd, response);
        return;
    }
    std::string message_to_send = "RSTT\r\n";
    std::string expected_response = "+OK recovering\r\n";
    if (create_socket_send_helper(serverID_to_revive, message_to_send, expected_response))
    {
        response = "+OK recovery requested\r\n";
        write_helper(socket_fd, response);
        verbose_print_helper_server(socket_fd, response);
        update_server_status_map_and_group_primary_map(serverID_to_revive);
    }
    else
    {
        response = "-ERR server unable to perform recovery\r\n";
        write_helper(socket_fd, response);
        verbose_print_helper_server(socket_fd, response);
    }
}

void QUIT_handler(int socket_fd, bool &quit_received)
{
    std::string response = "+OK Goodbye!\r\n";
    write_helper(socket_fd, response);
    verbose_print_helper_server(socket_fd, response);
    quit_received = true;
}

std::vector<std::string> get_list_of_active_secondaries(int group_no)
{
    std::vector<std::string> res;
    for (std::string serverID : tablet_storage_map.at(group_no))
    {
        if (serverID != group_primary_map.at(group_no) || server_status_map.at(serverID))
        {
            res.push_back(serverID);
        }
    }
    return res;
}

/// @brief Send PRIM:0,current_primary_ID to a secondary node
/// @param secondary_server_ID 
/// @param current_primary_ID 
/// @return true if successful, false if secondary did not send back expected response
bool send_PRIM_to_secondary(std::string secondary_server_ID, std::string current_primary_ID) {
    std::string expected_server_response = "+OK Primary updated\r\n";
    std::string message_to_send = "PRIM:0," + current_primary_ID;
    if (!create_socket_send_helper(secondary_server_ID, message_to_send, expected_server_response)) {
        return false;
    }
    return true;
}