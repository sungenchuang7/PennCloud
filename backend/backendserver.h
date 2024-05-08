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
#include <unordered_map>
#include <sstream>
#include <limits.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <memory>
#include <shared_mutex>
#include <mutex>
#include <thread>
#include <fcntl.h>
#include <queue>
#include <sys/file.h>
#include <sys/socket.h>

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

// Deallocates memory for the entire KVS.
void kvsCleanup();

// Logs a user requested action to disk.
void logActivity(int seqNum, char tablet, std::string action, std::string row, std::string column, std::string value, std::string length);

// Saves recent activities to disk.
void checkpointUpdate(char tablet);

// Worker thread that periodically updates disk storage depending on the updates log.
void* diskUpdatesThread(void* threadInfo);

// Loads a specified tablet into the KVS.
void importTablet(char tablet);

// Reads each tablet's record log to determine current activity count.
void loadRecordCounts();

// Requests the primary from the master for this node's replica group.
std::string requestPrimary();

// Sends a write request to the primary for replication.
bool writeToPrimary(std::string primary, std::string action, std::string row, std::string column, std::string* data);

// Sends a write request to all nodes in the replica group other than the primary.
void writeToGroup(std::string activeNodes, std::string action, std::string row, std::string column, std::string* data);