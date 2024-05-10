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

// Requests the primary from the master for this node's replica group.
std::string requestPrimary();

// Sends a write request to the primary for replication.
bool writeToPrimary(std::string primary, std::string action, std::string row, std::string column, std::string* data);

// Sends a write request to all nodes in the replica group other than the primary.
void writeToGroup(std::string activeNodes, std::string action, std::string row, std::string column, std::string* data);

// Enables recovery mode upon receiving request from backend master server.
void recovery();

// Collects each tablet's most recently checkpointed sequence number.
void loadRecordCounts();

// Find and load the last log file in memory.
std::vector<char> loadLogFile();

// Loads the specified checkpoint file to be sent to recovering node.
std::vector<char> loadCheckpointFile(char tablet);

// Update the specified log file with the new data.
void saveLogFile(char tablet, std::string* data);

// Update the specified checkpoint file with the new data.
void saveCheckpointFile(char tablet, std::string* data);

// Syncs the sequence numbers for this node with the primary's state.
void saveSequenceValues();

// Parse the log file's data to restore the state of the tablet.
void logFileRecovery();

// Sends the recovering node its missing log file data for the specified tablet.
void sendLogFileData(std::string node, std::vector<char>& data, char tablet);

// Sends the recovering node an updated checkpoint file.
void sendCheckpointFile(std::string node, std::vector<char>& data, char tablet);

// Send recovery request to primary.
void requestData(std::string primary, std::string updtArgument);

// Notify master of recovery completion.
void recoveryComplete();