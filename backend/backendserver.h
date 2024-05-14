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
// @param connectionInfo The details of the connection that the worker thread is assigned to service.
void* workerThread(void* connectionInfo);

struct threadDetails {
    int myIndex;  // This thread's index location in activeThreads.
    int connectionFD;  // The connection FD assigned to this thread.
};

// Activates server shutdown mode and notifies all threads when SIGINT caught.
// @param signal The signal to watch for.
void signalHandler(int signal);

// Deallocates memory for the entire KVS.
void kvsCleanup();

// Logs a user requested action to disk.
// @param seqNum The sequence number for the record.
// @param tablet The tablet for the record.
// @param action The action for the record (ex: PUT, DELE).
// @param row The row for the record.
// @param column The column for the record.
// @param value The value/data for the record.
// @param length The length of the value. Used to retrieve the value from the log.
void logActivity(int seqNum, char tablet, std::string action, std::string row, std::string column, std::string value, std::string length);

// Saves tablet updates to disk.
// @param tablet The tablet to be checkpointed.
void checkpointUpdate(char tablet);

// Worker thread that periodically performs checkpointing for the active tablet.
// @param threadInfo Any data passed to the thread.
void* diskUpdatesThread(void* threadInfo);

// Loads a specified tablet into the KVS.
// @param tablet The tablet to be imported.
void importTablet(char tablet);

// Requests the primary from the master for this node's replica group.
// @returns The primary's address.
std::string requestPrimary();

// Sends a write request to the primary for replication.
// @param primary The the address of the primary in the replica group.
// @param action The client requested action (ex: PUT, DELE).
// @param row The row associated with the client's request.
// @param column The column associated with the client's request.
// @param data The value associated with the client's request.
bool writeToPrimary(std::string primary, std::string action, std::string row, std::string column, std::string* data);

// Sends a write request to all nodes in the replica group other than the primary.
// @param activeNodes The list of all active nodes within this server's replica group.
// @param action The client requested action (ex: PUT, DELE).
// @param row The row associated with the client's request.
// @param column The column associated with the client's request.
// @param data The value associated with the client's request.
void writeToGroup(std::string activeNodes, std::string action, std::string row, std::string column, std::string* data);

// Enables recovery mode upon receiving request from backend master server.
void recovery();

// Collects each tablet's most recently checkpointed sequence number.
void loadRecordCounts();

// Find and load the last log file in memory.
// @returns A vector with the log file's data.
std::vector<char> loadLogFile();

// Loads the specified checkpoint file to be sent to recovering node.
// @param tablet The tablet associated with the desired checkpoint file.
// @returns A vector with the checkpoint file's data.
std::vector<char> loadCheckpointFile(char tablet);

// Update the specified log file with the new data.
// @param tablet The tablet associated with the specified log file.
// @param data The log file's data.
void saveLogFile(char tablet, std::string* data);

// Update the specified checkpoint file with the new data.
// @param tablet The tablet associated with the specified checkpoint file.
// @param data The checkpoint file's data.
void saveCheckpointFile(char tablet, std::string* data);

// Syncs the sequence numbers for this node with the primary's state.
void saveSequenceValues();

// Parse the log file's data to restore the state of the tablet.
// @param logFile The log file used to recover the lost transactions.
void logFileRecovery(std::vector<char> logFile);

// Sends the recovering node its missing log file data for the specified tablet.
// @param node The recovering node's address.
// @param data The log file's data.
// @param tablet The tablet associated with the log file.
void sendLogFileData(std::string node, std::vector<char>& data, char tablet);

// Sends the recovering node an updated checkpoint file.
// @param node The recovering node's address.
// @param data The checkpoint file's contents.
// @param tablet The tablet associated with the checkpoint file.
void sendCheckpointFile(std::string node, std::vector<char>& data, char tablet);

// Send recovery request to primary.
// @param primary The primary's address.
// @param updtArgument This highest relevant sequence numbers recorded for this node.
void requestData(std::string primary, std::string updtArgument);

// Notify master of recovery completion.
void recoveryComplete();

// Update sequence numbers on disk during recovery.
// @param primaryResponse The new sequence numbers to be referenced at this node.
void updateSequenceNumbers(std::vector<char> primaryResponse);