#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <iostream>
#include <vector>
#include <fstream>
#include <sys/wait.h>
#include <string.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <dirent.h>
#include <sys/types.h>
#include <unordered_map>
#include <sys/file.h>
#include <chrono>
#include <ctime>

// Parses input arguments and sets appropriate flag values.
// @param argc  The argument count.
// @param argv  The argument value.
int parseArgs(int argc, char *argv[]);

// Manages client connections and thread allocations.
void connectionManager();

// Dispatch thread to handle the specified connection.
void* workerThread(void* connectionInfo);

struct threadDetails {
    int myIndex;  // This thread's index location in activeThreads.
    int connectionFD;  // The connection FD assigned to this thread.
};

// Activates server shutdown mode and notifies all threads when SIGINT caught.
void signalHandler(int signal);