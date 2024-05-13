#include "backendserver.h"

bool vFlag = false, rFlag = false;  // Command line argument flags.
bool currentPrimary = false;  // Whether or not this node is the current primary.
std::string primaryAddress;  // Stores the current primary's address.
int portNumber = 10000;  // This server's port number.
int myIndex = 1;  // Stores the index of this node within the addresses list.
int masterPort;  // Stores the master's port number.
std::vector<std::string> activeNodes;  // Stores the active nodes for this replica group.
std::string masterIP;; // Stores the master's IP address.
std::vector<std::string> ipPorts;  // The IP:Port values for each node.
std::string myTablets;  // The tablets that this server is responsible for.
std::unordered_map<char, std::string> recordCounts;  // Stores the last sequence number that was checkpointed for each tablet.

std::unordered_map<std::string, std::unordered_map<std::string, std::string*>> keyValueStore; // Key-value store for this node's rows and backup storage.
int sequenceNumber = 0;  // The total number of requests processed on this node. Sequence number.
char activeTablet = '0';  // The tablet currently loaded into memory.

std::vector<int> availableThreadIndices;  // Thread indices currently available for creation.
std::vector<int> closedConnections;  // Stores thread indices associated with recently closed connections.
pthread_t activeThreads[100];  // Stores active threads for connection handling. Only accessed by main thread.
pthread_t writerThread;  // The thread responsible for writing to disk.
pthread_mutex_t threadUpdatesLock;  // Lock for accessing availableThreadIndices and closedConnections.
pthread_mutex_t sequenceNumberLock;  // Lock for accessing sequenceNumber.
pthread_mutex_t primaryUpdateLock;  // Lock for accessing primary status and active nodes.
pthread_mutex_t activeTabletLock;  // Lock for accessing activeTablet. Required when swapping tablets.
std::shared_timed_mutex readWriteLock;  // Lock for extending read and write permissions for the active tablet.

int shutDownPipe[2];  // Pipe for communicating shutdown status to threads.
bool serverShutDown = false;  // Tracks Ctrl+C command from user.
bool shutDownCleanup = false;  // Signals when thread resources have been cleaned up.
bool pseudoShutDown = false;  // Shutdown request sent from frontend. Reject non-recovery requests.
bool recoveryMode = false;  // Keeps track of node's recovery process.
bool logOrCheckpoint = false;  // Tracks whether or not the incoming data is for a checkpoint log or checkpoint file.
char lastLogFile = '0';  // The last log file that failed to checkpoint.
char recoveringTablet = '0';  // The tablet currently undergoing recovery.

std::string serverGreeting = "+OK Server ready\r\n";
std::string invalidCommand = "-ERR Unknown command\r\n";
std::string invalidArgument = "-ERR Invalid argument\r\n";
std::string invalidSequenceOfCommands = "-ERR Invalid sequence of commands\r\n";
std::string quitMessage = "+OK Goodbye!\r\n";
std::string serverShutDownMessage = "-ERR Server shutting down\r\n";
std::string putOkay = "+OK Send value with DATA\r\n";
std::string okayMessage = "+OK\r\n";
std::string invalidValue = "-ERR Value does not exist\r\n";
std::string cputOkay = "+OK Send first value with DATA\r\n";
std::string deleteOkay = "+OK Value deleted\r\n";
std::string valueAdded = "+OK Value added\r\n";
std::string dataOkay = "+OK Enter value ending with <CRLF>.<CRLF>\r\n";
std::string secondValueOkay = "+OK Enter second value with DATA\r\n";
std::string firstValueInvalid = "-ERR Value does not equal current value\r\n";
std::string primMessage = "+OK Primary updated\r\n";
std::string stdnResponse = "+OK shutting down\r\n";
std::string rsttResponse = "+OK recovering\r\n";

// Main entry point for this program. See backendserver.h for function documentation.
// @returns Exit code 0 for success, else error.
int main(int argc, char *argv[]) {
    // Parse arguments and set flags.
    parseArgs(argc, argv);

    // Watch for user's Ctrl+C signal.
    struct sigaction sigAction;
    sigAction.sa_handler = signalHandler;
    sigemptyset(&sigAction.sa_mask);
    sigAction.sa_flags = 0;
    sigaction(SIGINT, & sigAction, 0);

    // Initialize locks.
    pthread_mutex_init(&threadUpdatesLock, NULL);
    pthread_mutex_init(&sequenceNumberLock, NULL);
    pthread_mutex_init(&activeTabletLock, NULL);
    pthread_mutex_init(&primaryUpdateLock, NULL);

    // Establish connections and dispatch threads to handle them.
    connectionManager();

    return 0;
}

int parseArgs(int argc, char *argv[]) {
    int optValue = 0;
    std::string configFile = "";
    std::string nextLine = "";
    std::string newPortNumber = "";
    bool portTracker = false;

    // No command line options enabled.
    if (argc == 1) {
        return 0;
    }

    // Check for specified options.
    while ((optValue = getopt(argc, argv, "c:i:rv")) != -1) {
        if (optValue == 'c') {
            // Config enabled.
            configFile = optarg;
        } else if (optValue == 'v') {
            // -v option enabled.
            vFlag = true;
        } else if (optValue == 'i') {
            // Set node index identifier.
            std::string newIndex = optarg;
            myIndex = std::stoi(newIndex);
        }  else if (optValue == 'r') {
            // Recovery option enabled.
            rFlag = true;
        }
    }

    // Gather the IP:Port values from the config file.
    if (configFile != "") {
        std::ifstream fileStream(configFile);

        // Parse each line in file.
        while (getline(fileStream, nextLine)) {
            ipPorts.push_back(nextLine);
            nextLine.clear();
        }

        fileStream.close();
    }

    // Update this server's port number.
    for (int i = 0; i < ipPorts[myIndex].length(); i++) {
        if (portTracker == false && ipPorts[myIndex][i] == ':') {
            portTracker = true;
        } else if (portTracker == true) {
            newPortNumber += ipPorts[myIndex][i];
        }
    }

    portNumber = std::stoi(newPortNumber);

    // Store the master's IP and port.
    std::string masterValue;
    portTracker = false;

    for (int i = 0; i < ipPorts[0].length(); i++) {
        if (portTracker == false && ipPorts[0][i] == ':') {
            portTracker = true;
        } else if (portTracker == true) {
            masterValue += ipPorts[0][i];
        } else {
            masterIP += ipPorts[0][i];
        }
    }

    masterPort = std::stoi(masterValue);

    // Update primary information.
    if (myIndex == 1) {
        currentPrimary = true;
        activeNodes.push_back(ipPorts[1]);
        activeNodes.push_back(ipPorts[2]);
        activeNodes.push_back(ipPorts[3]);
    } else if (myIndex == 4) {
        currentPrimary = true;
        activeNodes.push_back(ipPorts[4]);
        activeNodes.push_back(ipPorts[5]);
        activeNodes.push_back(ipPorts[6]);
    } else if (myIndex == 7) {
        currentPrimary = true;
        activeNodes.push_back(ipPorts[7]);
        activeNodes.push_back(ipPorts[8]);
        activeNodes.push_back(ipPorts[9]);
    }

    // Assign range of tablets and primary address to this node. Used for writes and recovery.
    if (myIndex == 1 || myIndex == 2 || myIndex == 3) {
        myTablets = "abcdefghi";
        primaryAddress = ipPorts[1];
    } else if (myIndex == 4 || myIndex == 5 || myIndex == 6) {
        myTablets = "jklmnopqr";
        primaryAddress = ipPorts[4];
    } else if (myIndex == 7 || myIndex == 8 || myIndex == 9) {
        myTablets = "stuvwxyz";
        primaryAddress = ipPorts[7];
    }

    return 0;
}

void connectionManager() {
    // Create a new socket.
    int listen_FD = socket(PF_INET, SOCK_STREAM, 0);
    struct sockaddr_in servaddr;
    bzero(&servaddr, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htons(INADDR_ANY);
    servaddr.sin_port = htons(portNumber);
    struct timeval timeTracker;  // Wait time before thread cleanup.
    fd_set fdSet;

    // Allow port to be reused right after connection ends.
    int opt = 1;
    int ret = setsockopt(listen_FD, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt));

    // Set up shutdown pipes.
    pipe(shutDownPipe);

    // Set up accept() timer.
    timeTracker.tv_sec = 2;
    timeTracker.tv_usec = 0;

    if (listen_FD < 0) {
        fprintf(stderr, "Fail to open socket: %s\n", strerror(errno));
        exit(1);
    }

    if (bind(listen_FD, (struct sockaddr*)&servaddr, sizeof(servaddr)) < 0) {
        fprintf(stderr, "Fail to bind: %s\n", strerror(errno));
        exit(1);
    }

    // Connect to the server.
    if (listen(listen_FD, 100) < 0) {
        fprintf(stderr, "Fail to listen: %s\n", strerror(errno));
        exit(1);
    }

    // Make all thread indices available, initialize fileDescriptors.
    for (int i = 0; i < 100; i++) {
        availableThreadIndices.push_back(i);
    }

    // Assign thread to handle writing logged activities to disk.
    pthread_create(&writerThread, NULL, diskUpdatesThread, nullptr);

    // Loop until server is shut down.
    while (serverShutDown == false) {
        // If a thread is available, dispatch it, else wait for a thread to be available.
        if (availableThreadIndices.size() > 0) {
            // Connect to the server and set up thread details.
            struct sockaddr_in clientaddr;
            socklen_t clientaddrlen = sizeof(clientaddr);
            threadDetails* nextThreadDetails = new threadDetails;
            nextThreadDetails->connectionFD = -1;

            FD_ZERO(&fdSet);
            FD_SET(listen_FD, &fdSet);
            int returnValue = select(listen_FD + 1, &fdSet, NULL, NULL, &timeTracker);

            if (returnValue > 0) {
                // Connection is available, accept and set thread details.
                nextThreadDetails->connectionFD = accept(listen_FD, (struct sockaddr*)&clientaddr, &clientaddrlen);
                nextThreadDetails->myIndex = availableThreadIndices.front();
                availableThreadIndices.erase(availableThreadIndices.begin());
            }

            // Create new thread with new connection.
            if (serverShutDown == false && nextThreadDetails->connectionFD != -1) {
                // Dispatch thread to handle this connection.
                pthread_create(&activeThreads[nextThreadDetails->myIndex], NULL, workerThread, nextThreadDetails);
            } else {
                // Don't create thread, continue to clean up.
                delete nextThreadDetails;
            }
        }

        // Handle closed connections.
        if (serverShutDown == false) {
            pthread_mutex_lock(&threadUpdatesLock);
            if (closedConnections.size() > 0) {
                // Join threads with closed connections.
                for (int i = 0; i < closedConnections.size(); i++) {
                    if (pthread_join(activeThreads[closedConnections[i]], nullptr) != 0) {
                        std::cerr << "pthread_join failed." << std::endl;
                    }
                    availableThreadIndices.push_back(closedConnections[i]);
                }

                // Empty closedConnections vector.
                closedConnections.clear();

                pthread_mutex_unlock(&threadUpdatesLock);
            } else {
                pthread_mutex_unlock(&threadUpdatesLock);
            }
        }
    }

    // Server shutdown enabled. Wait and join all threads.
    while (shutDownCleanup == false) {
        pthread_mutex_lock(&threadUpdatesLock);
        if (closedConnections.size() > 0) {
            // Join threads with closed connections.
            for (int i = 0; i < closedConnections.size(); i++) {
                if (pthread_join(activeThreads[closedConnections[i]], nullptr) != 0) {
                    std::cerr << "pthread_join failed." << std::endl;
                }
                availableThreadIndices.push_back(closedConnections[i]);
            }

            // Empty closedConnections vector.
            closedConnections.clear();

            pthread_mutex_unlock(&threadUpdatesLock);
        } else {
            pthread_mutex_unlock(&threadUpdatesLock);
        }

        // Thread cleanup finished. Update signal.
        if (availableThreadIndices.size() == 100) {
            shutDownCleanup = true;
        }
    }

    // Wait for writer thread to exit.
    if (pthread_join(writerThread, nullptr) != 0) {
        std::cerr << "pthread_join failed." << std::endl;
    }

    // Close file descriptors.
    close(listen_FD);
    close(shutDownPipe[0]);
    close(shutDownPipe[1]);

    // Lock cleanup.
    pthread_mutex_destroy(&threadUpdatesLock);
    pthread_mutex_destroy(&sequenceNumberLock);
    pthread_mutex_destroy(&activeTabletLock);
    pthread_mutex_destroy(&primaryUpdateLock);

    // Deallocate KVS memory.
    kvsCleanup();
}

void* workerThread(void* connectionInfo) {
    std::vector<char> buf;
    char targetTablet;
    // char recoveringTablet = '0';  // The tablet currently undergoing recovery.
    int comm_FD = static_cast<threadDetails*>(connectionInfo)->connectionFD;
    int numRead = 0;  // The number of bytes returned by read().
    int numWrite = 0; // The number of bytes written by write();
    bool quitCalled = false;
    bool continueReading = false;
    bool clientDisconnected = false;
    bool dataCalled = false;  // Tracks value input mode.
    bool cputSecondPass = false;  // Tracks whether or not v_1 is equal to the existing value.
    bool rwLockPickedUp = false;
    bool writeReceived = false;  // Tracks when we are processing a write from primary.
    bool pwrtReceived = false;  // Tracks when the primary is accepting data to be propogated.
    // bool logOrCheckpoint = false;  // Tracks whether or not the incoming data is for a checkpoint log or checkpoint file.
    std::string requestedCommand = "Default";  // The client's most recently requested command.
    std::string requestedRow = "";
    std::string requestedColumn = "";
    std::string* valueData = new std::string("");  // Stores the value sent via the DATA command.
    std::string recoveringNode = "";

    // Write greeting to client.
    write(comm_FD, &serverGreeting[0], serverGreeting.length());

    // Maintain connection with client and execute commands.
    while (quitCalled == false) {
        // Read user input into intermediate array.
        std::vector<char> tempBuf(20000);
        fd_set fdSet;  // Used to keep track of both client input and the server shutdown signal.

        FD_ZERO(&fdSet);
        FD_SET(comm_FD, &fdSet);
        FD_SET(shutDownPipe[0], &fdSet);

        // DATA processing mode.
        if (dataCalled == true && buf.size() > 0) {
            bool terminate = false;
            std::vector<char> dataToAdd;
            int lastIndex = 0;

            // Perform pass over data to look for terminating values.
            for (int i = 0; i < buf.size(); i++) {
                if (buf[i] == '\r' &&
                    buf.size() - 1 - i >= 4 &&
                    buf[i + 1] == '\n' &&
                    buf[i + 2] == '.' &&
                    buf[i + 3] == '\r' &&
                    buf[i + 4] == '\n') {
                    // All terminating values in read buffer.
                    terminate = true;
                    lastIndex = i + 4;
                }
            }

            if (terminate == false) {
                // Offload portion of buffer to valueData, or skip and read again.
                if (buf.size() >= 15000) {
                    // Offload 10000 characters.
                    dataToAdd.insert(dataToAdd.begin(), buf.begin(), buf.begin() + 10000);
                    valueData->append(dataToAdd.begin(), dataToAdd.end());

                    // Remove characters from buffer that were added to valueData.
                    std::vector<char> dataTempArray;
                    dataTempArray.insert(dataTempArray.begin(), buf.begin(), buf.end());
                    buf.clear();
                    int dataBufPosition = 0;

                    for (int j = 10000; j < dataTempArray.size(); j++) {
                        buf.push_back(dataTempArray[j]);
                    }
                }
            } else {
                // DATA content complete. Gather last string.
                dataToAdd.insert(dataToAdd.end(), buf.begin(), buf.begin() + lastIndex - 4);
                valueData->append(dataToAdd.begin(), dataToAdd.end());

                if (requestedCommand == "CPUT" && cputSecondPass == false) {
                    // Compare v_1 to existing value.
                    if (keyValueStore[requestedRow][requestedColumn] != nullptr && *valueData == *keyValueStore[requestedRow][requestedColumn]) {
                        // Current value = v_1, v_2 can now be accepted.
                        cputSecondPass = true;
                        *valueData = "";

                        if (write(comm_FD, &secondValueOkay[0], secondValueOkay.length()) < 0) {
                            fprintf(stderr, "DATA failed to write: %s\n", strerror(errno));
                        }
                    } else {
                        // v_1 does not equal current value. End transaction.
                        if (write(comm_FD, &firstValueInvalid[0], firstValueInvalid.length()) < 0) {
                            fprintf(stderr, "DATA failed to write: %s\n", strerror(errno));
                        }

                        requestedCommand = "Default";
                        *valueData = "";

                        // Release locks.
                        if (rwLockPickedUp == true) {
                            pthread_mutex_unlock(&activeTabletLock);
                            readWriteLock.unlock();
                            rwLockPickedUp = false;
                        } else {
                            fprintf(stderr, "Locks logic incorrect for CPUT DATA.\n");
                        }
                    }
                } else if (pwrtReceived == true) {
                    sequenceNumber++;
                    logActivity(sequenceNumber, targetTablet, requestedCommand, requestedRow, requestedColumn, *valueData, std::to_string(valueData->length()));
                    keyValueStore[requestedRow][requestedColumn] = valueData;

                    // Add nodes to list for parsing.
                    std::string nodeList = "";
                    for (int i = 0; i < activeNodes.size(); i++) {
                        nodeList += activeNodes[i];
                        if (i != activeNodes.size() - 1) {
                            nodeList += ',';
                        }
                    }

                    writeToGroup(nodeList, "PUT", requestedRow, requestedColumn, valueData);

                    if (write(comm_FD, &valueAdded[0], valueAdded.length()) < 0) {
                        fprintf(stderr, "DATA failed to write: %s\n", strerror(errno));
                    }

                    valueData = new std::string("");
                    cputSecondPass = false;
                    pwrtReceived = false;
                    requestedCommand = "Default";

                    // Release locks.
                    if (rwLockPickedUp == true) {
                        pthread_mutex_unlock(&activeTabletLock);
                        readWriteLock.unlock();
                        rwLockPickedUp = false;
                    } else {
                        fprintf(stderr, "Locks logic incorrect for PWRT DATA.\n");
                    }
                } else if (writeReceived == true) {
                    // Write received from primary. Log activity and write data to KVS.
                    if (keyValueStore[requestedRow][requestedColumn] != nullptr) {
                        delete keyValueStore[requestedRow][requestedColumn];
                    }

                    logActivity(sequenceNumber, targetTablet, "PUT", requestedRow, requestedColumn, *valueData, std::to_string(valueData->length()));
                    keyValueStore[requestedRow][requestedColumn] = valueData;

                    if (write(comm_FD, &valueAdded[0], valueAdded.length()) < 0) {
                        fprintf(stderr, "DATA failed to write: %s\n", strerror(errno));
                    }

                    valueData = new std::string("");
                    requestedCommand = "Default";
                    cputSecondPass = false;
                    writeReceived = false;

                    // Release locks.
                    if (rwLockPickedUp == true) {
                        pthread_mutex_unlock(&activeTabletLock);
                        readWriteLock.unlock();
                        rwLockPickedUp = false;
                    } else {
                        fprintf(stderr, "Lock logic incorrect WRIT DATA.\n");
                    }
                } else if (recoveryMode == true && logOrCheckpoint == false) {
                    // Write data to log file.
                    saveLogFile(recoveringTablet, valueData);
                    delete valueData;
                    valueData = new std::string("");
                    logOrCheckpoint = false;

                    if (write(comm_FD, &valueAdded[0], valueAdded.length()) < 0) {
                        fprintf(stderr, "DATA failed to write: %s\n", strerror(errno));
                    }
                } else if (recoveryMode == true && logOrCheckpoint == true) {

                    // Write data to checkpoint file.
                    saveCheckpointFile(recoveringTablet, valueData);
                    delete valueData;
                    valueData = new std::string("");
                    logOrCheckpoint = false;

                    if (write(comm_FD, &valueAdded[0], valueAdded.length()) < 0) {
                        fprintf(stderr, "DATA failed to write: %s\n", strerror(errno));
                    }
                } else {
                    // PUT or CPUT data received.

                    // Release locks to enable primary's write request.
                    if (rwLockPickedUp == true) {
                        pthread_mutex_unlock(&activeTabletLock);
                        readWriteLock.unlock();
                        rwLockPickedUp = false;
                    } else {
                        fprintf(stderr, "Locks not picked up correctly for PUT/CPUT DATA.\n");
                    }

                    // Get and send write request to primary.
                    writeToPrimary(primaryAddress, "PUT", requestedRow, requestedColumn, valueData);

                    valueData = new std::string("");
                    requestedCommand = "Default";
                    cputSecondPass = false;
                    writeReceived = false;

                    // Message added, respond to server.
                    if (write(comm_FD, &valueAdded[0], valueAdded.length()) < 0) {
                        fprintf(stderr, "DATA failed to write: %s\n", strerror(errno));
                    }
                }

                // Clean buffer.
                std::vector<char> dataTempArray;
                dataTempArray.insert(dataTempArray.begin(), buf.begin(), buf.end());
                buf.clear();
                int dataBufPosition = 0;

                if (lastIndex < 19999) {
                    for (int j = lastIndex + 1; j < dataTempArray.size(); j++) {
                        buf.push_back(dataTempArray[j]);
                    }
                }

                // Exit DATA processing.
                dataCalled = false;
            }
        }

        // When shutdown pipe read available, enter shutdown mode.
        int returnValue = select(comm_FD + 1, &fdSet, NULL, NULL, NULL);

        // Server shutdown enabled.
        if (serverShutDown == true) {
            break;
        }

        numRead = read(comm_FD, tempBuf.data(), 2000);

        // Client has disconnected without requesting QUIT or read() failed. Terminate thread.
        if (numRead <= 0) {
            clientDisconnected = true;
            break;
        }
        
        // Copy contents of temp array into full array.
        buf.insert(buf.end(), tempBuf.begin(), tempBuf.begin() + numRead);

        // Check for complete commands in buffer. Exit loop when no commands remain.
        while (continueReading == false && quitCalled == false && dataCalled == false) {
            for (int i = 0; i < buf.size() - 1; i++) {
                // Last possible command in buffer. Continue reading after loop breaks.
                if (i == buf.size() - 2) {
                    continueReading = true;
                }

                if (buf[i] == '\r') {
                    if (i < buf.size() - 1) {
                        if (buf[i + 1] == '\n') {
                            // String is complete. Check for command.

                            if (buf.size() < 6) {
                                // Invalid command received.
                                if (write(comm_FD, &invalidCommand[0], invalidCommand.length()) < 0) {
                                    fprintf(stderr, "Failed to write: %s\n", strerror(errno));
                                }
                            } else if ((buf.size() > 5) &&
                                    (buf[0] == 'q' || buf[0] == 'Q') && 
                                    (buf[1] == 'u' || buf[1] == 'U') && 
                                    (buf[2] == 'i' || buf[2] == 'I') &&
                                    (buf[3] == 't' || buf[3] == 'T') &&
                                    (buf[4] == '\r') &&
                                    (buf[5] == '\n')) {
                                // Pseudo shutdown enabled. Reject request.
                                if (pseudoShutDown == true && recoveryMode == false) {
                                    break;
                                }

                                // QUIT called, break from loops.
                                quitCalled = true;

                                break;
                            } else if ((buf.size() > 5) &&
                                    (buf[0] == 'd' || buf[0] == 'D') && 
                                    (buf[1] == 'a' || buf[1] == 'A') && 
                                    (buf[2] == 't' || buf[2] == 'T') &&
                                    (buf[3] == 'a' || buf[3] == 'A') &&
                                    (buf[4] == '\r') &&
                                    (buf[5] == '\n')) {
                                // Pseudo shutdown enabled. Reject request.
                                if (pseudoShutDown == true && recoveryMode == false) {
                                    break;
                                }

                                // DATA called, break loop and enter value parsing mode.
                                if (requestedCommand == "PUT" || requestedCommand == "CPUT" || recoveryMode == true || requestedCommand == "RECOVER") {
                                    // Data call accepted.
                                    dataCalled = true;

                                    if (write(comm_FD, &dataOkay[0], dataOkay.length()) < 0) {
                                        fprintf(stderr, "DATA acceptance failed to write: %s\n", strerror(errno));
                                    }
                                } else {
                                    // Invalid sequence of commands.
                                    if (write(comm_FD, &invalidSequenceOfCommands[0], invalidSequenceOfCommands.length()) < 0) {
                                        fprintf(stderr, "DATA acceptance failed to write: %s\n", strerror(errno));
                                    }
                                }
                            } else if ((buf.size() > 8) &&
                                       (buf[0] == 'p' || buf[0] == 'P') &&
                                       (buf[1] == 'u' || buf[1] == 'U') &&
                                       (buf[2] == 't' || buf[2] == 'T') &&
                                       (buf[3] == ':')) {
                                // Pseudo shutdown enabled. Reject request.
                                if (pseudoShutDown == true) {
                                    break;
                                }

                                // PUT() called. Extract the argument and test for validity.
                                std::string putArgument = "";
                                bool colTracker = false;
                                putArgument.append(buf.begin() + 4, buf.begin() + i);
                                requestedRow = "";
                                requestedColumn = "";

                                for (int k = 0; k < putArgument.length(); k++) {
                                    if (putArgument[k] == '\r') {
                                        break;
                                    } else if (colTracker == false && putArgument[k] != ':') {
                                        // Add to row value.
                                        requestedRow += putArgument[k];
                                    } else if (colTracker == true) {
                                        // Add to column value.
                                        requestedColumn += putArgument[k];
                                    } else if (putArgument[k] == ':') {
                                        colTracker = true;
                                    }
                                }

                                targetTablet = std::tolower(requestedRow[0]);

                                if ((targetTablet - 'a' >= 0) && (targetTablet - 'a' <= 25)) {
                                    // Pick up lock for this tablet.
                                    pthread_mutex_lock(&activeTabletLock);
                                    readWriteLock.lock();
                                    rwLockPickedUp = true;
                                } else {
                                    fprintf(stderr, "Error: Invalid tablet requested for PUT.\n");
                                }

                                if (requestedRow.length() == 0 || requestedColumn.length() == 0) {
                                    // Invalid argument entered. Notify client.
                                    if (write(comm_FD, &invalidArgument[0], invalidArgument.length()) < 0) {
                                        fprintf(stderr, "PUT failed to write: %s\n", strerror(errno));
                                    }

                                    // Release locks.
                                    if (rwLockPickedUp == true) {
                                        pthread_mutex_unlock(&activeTabletLock);
                                        readWriteLock.unlock();
                                        rwLockPickedUp = false;
                                    }
                                } else {
                                    // Argument valid. Update state and notify client.
                                    requestedCommand = "PUT";

                                    if (write(comm_FD, &putOkay[0], putOkay.length()) < 0) {
                                        fprintf(stderr, "PUT failed to write: %s\n", strerror(errno));
                                    }
                                }
                            } else if ((buf.size() > 8) &&
                                       (buf[0] == 'g' || buf[0] == 'G') &&
                                       (buf[1] == 'e' || buf[1] == 'E') &&
                                       (buf[2] == 't' || buf[2] == 'T') &&
                                       (buf[3] == ':')) {
                                // Pseudo shutdown enabled. Reject request.
                                if (pseudoShutDown == true) {
                                    break;
                                }

                                // GET() called. Extract the argument and test for validity.
                                std::string getArgument = "";
                                bool colTracker = false;
                                getArgument.append(buf.begin() + 4, buf.begin() + i);
                                requestedRow = "";
                                requestedColumn = "";
                                requestedCommand = "DEFAULT";

                                for (int k = 0; k < getArgument.length(); k++) {
                                    if (getArgument[k] == '\r') {
                                        break;
                                    } else if (colTracker == false && getArgument[k] != ':') {
                                        // Add to row value.
                                        requestedRow += getArgument[k];
                                    } else if (colTracker == true) {
                                        // Add to column value.
                                        requestedColumn += getArgument[k];
                                    } else if (getArgument[k] == ':') {
                                        colTracker = true;
                                    }
                                }

                                // Swap tablet if necessary and pick up shared lock.
                                targetTablet = std::tolower(requestedRow[0]);

                                if ((targetTablet - 'a' >= 0) && (targetTablet - 'a' <= 25)) {
                                    // Swap tablet if necessary and pick up shared lock.
                                    pthread_mutex_lock(&activeTabletLock);
                                    readWriteLock.lock_shared();
                                    rwLockPickedUp = true;
                                    if (activeTablet != targetTablet) {
                                        importTablet(targetTablet);
                                        activeTablet = targetTablet;
                                    }

                                    pthread_mutex_unlock(&activeTabletLock);
                                } else {
                                    fprintf(stderr, "Error: Invalid tablet requested for GET.\n");
                                }

                                if (requestedRow.length() == 0 || requestedColumn.length() == 0) {
                                    if (write(comm_FD, &invalidArgument[0], invalidArgument.length()) < 0) {
                                        fprintf(stderr, "GET failed to write: %s\n", strerror(errno));
                                    }
                                } else if (keyValueStore[requestedRow][requestedColumn] != nullptr) {
                                    // Send the requested value to user.
                                    std::string endOutput = "\r\n.\r\n";
                                    
                                    if (write(comm_FD, &okayMessage[0], okayMessage.length()) < 0) {
                                        fprintf(stderr, "GET failed to write: %s\n", strerror(errno));
                                    }
                                    if (write(comm_FD, keyValueStore[requestedRow][requestedColumn]->data(), keyValueStore[requestedRow][requestedColumn]->length()) < 0) {
                                        fprintf(stderr, "GET failed to write: %s\n", strerror(errno));
                                    }
                                    if (write(comm_FD, &endOutput[0], endOutput.length()) < 0) {
                                        fprintf(stderr, "GET failed to write: %s\n", strerror(errno));
                                    }
                                } else {
                                    if (write(comm_FD, &invalidValue[0], invalidValue.length()) < 0) {
                                        fprintf(stderr, "GET failed to write to client: %s\n", strerror(errno));
                                    }
                                }

                                // Release lock.
                                if (rwLockPickedUp == true) {
                                    readWriteLock.unlock_shared();
                                    rwLockPickedUp = false;
                                }
                            } else if ((buf.size() > 9) &&
                                       (buf[0] == 'c' || buf[0] == 'C') &&
                                       (buf[1] == 'p' || buf[1] == 'P') &&
                                       (buf[2] == 'u' || buf[2] == 'U') &&
                                       (buf[3] == 't' || buf[3] == 'T') &&
                                       (buf[4] == ':')) {
                                // Pseudo shutdown enabled. Reject request.
                                if (pseudoShutDown == true) {
                                    break;
                                }

                                // CPUT() called. Extract the argument and test for validity.
                                std::string cputArgument = "";
                                bool colTracker = false;
                                cputArgument.append(buf.begin() + 5, buf.begin() + i);
                                requestedRow = "";
                                requestedColumn = "";

                                for (int k = 0; k < cputArgument.length(); k++) {
                                    if (cputArgument[k] == '\r') {
                                        break;
                                    } else if (colTracker == false && cputArgument[k] != ':') {
                                        // Add to row value.
                                        requestedRow += cputArgument[k];
                                    } else if (colTracker == true) {
                                        // Add to column value.
                                        requestedColumn += cputArgument[k];
                                    } else if (cputArgument[k] == ':') {
                                        colTracker = true;
                                    }
                                }

                                targetTablet = std::tolower(requestedRow[0]);

                                if ((targetTablet - 'a' >= 0) && (targetTablet - 'a' <= 25)) {
                                    // Pick up lock for this tablet.
                                    pthread_mutex_lock(&activeTabletLock);
                                    readWriteLock.lock();
                                    rwLockPickedUp = true;

                                    if (activeTablet != targetTablet) {
                                        importTablet(targetTablet);
                                        activeTablet = targetTablet;
                                    }
                                } else {
                                    fprintf(stderr, "Error: Invalid tablet requested for DATA.\n");
                                }

                                if (requestedRow.length() == 0 || requestedColumn.length() == 0) {
                                    // Invalid argument.
                                    if (write(comm_FD, &invalidArgument[0], invalidArgument.length()) < 0) {
                                        fprintf(stderr, "CPUT failed to write: %s\n", strerror(errno));
                                    }

                                    // Release locks.
                                    if (rwLockPickedUp == true) {
                                        pthread_mutex_unlock(&activeTabletLock);
                                        readWriteLock.unlock();
                                        rwLockPickedUp = false;
                                    }
                                } else {
                                    // Argument valid, update state and notify user.
                                    requestedCommand = "CPUT";

                                    if (write(comm_FD, &cputOkay[0], cputOkay.length()) < 0) {
                                        fprintf(stderr, "CPUT failed to write: %s\n", strerror(errno));
                                    }
                                }
                            } else if ((buf.size() > 9) &&
                                       (buf[0] == 'd' || buf[0] == 'D') &&
                                       (buf[1] == 'e' || buf[1] == 'E') &&
                                       (buf[2] == 'l' || buf[2] == 'L') &&
                                       (buf[3] == 'e' || buf[3] == 'E') &&
                                       (buf[4] == ':')) {
                                // Pseudo shutdown enabled. Reject request.
                                if (pseudoShutDown == true) {
                                    break;
                                }

                                // DELETE() called. Extract the argument and test for validity.
                                std::string deleArgument = "";
                                bool colTracker = false;
                                deleArgument.append(buf.begin() + 5, buf.begin() + i);
                                requestedRow = "";
                                requestedColumn = "";
                                requestedCommand = "DEFAULT";

                                for (int k = 0; k < deleArgument.length(); k++) {
                                    if (deleArgument[k] == '\r') {
                                        break;
                                    } else if (colTracker == false && deleArgument[k] != ':') {
                                        // Add to row value.
                                        requestedRow += deleArgument[k];
                                    } else if (colTracker == true) {
                                        // Add to column value.
                                        requestedColumn += deleArgument[k];
                                    } else if (deleArgument[k] == ':') {
                                        colTracker = true;
                                    }
                                }

                                if (requestedRow.length() == 0 || requestedColumn.length() == 0) {
                                    // Invalid argument.
                                    if (write(comm_FD, &invalidArgument[0], invalidArgument.length()) < 0) {
                                        fprintf(stderr, "DELE failed to write: %s\n", strerror(errno));
                                    }
                                } else {
                                    // Argument accepted, send request to primary.
                                    targetTablet = std::tolower(requestedRow[0]);
                                    if (((targetTablet - 'a' >= 0) && (targetTablet - 'a' <= 25)) == false) {
                                        fprintf(stderr, "Error: Invalid tablet requested for DELE.\n");
                                    }

                                    bool err = writeToPrimary(primaryAddress, "DELE", requestedRow, requestedColumn, nullptr);

                                    if (err == false) {
                                        // Value deleted.
                                        if (write(comm_FD, &deleteOkay[0], deleteOkay.length()) < 0) {
                                            fprintf(stderr, "DELE failed to write: %s\n", strerror(errno));
                                        }
                                    } else {
                                        // Value does not exist.
                                        if (write(comm_FD, &invalidValue[0], invalidValue.length()) < 0) {
                                            fprintf(stderr, "DELE failed to write: %s\n", strerror(errno));
                                        }
                                    }
                                }
                            } else if ((buf.size() > 11) &&
                                       (buf[0] == 'p' || buf[0] == 'P') &&
                                       (buf[1] == 'w' || buf[1] == 'W') &&
                                       (buf[2] == 'r' || buf[2] == 'R') &&
                                       (buf[3] == 't' || buf[3] == 'T') &&
                                       (buf[4] == ':')) {
                                // Pseudo shutdown enabled. Reject request.
                                if (pseudoShutDown == true) {
                                    break;
                                }

                                // WRITE request received from a secondary.
                                std::string pwrtArgument = "";
                                requestedCommand = "";
                                requestedRow = "";
                                requestedColumn = "";
                                int indexTracker = 0;
                                pwrtArgument.append(buf.begin() + 5, buf.begin() + i);
                                pwrtReceived = true;

                                // Parse the arguments.
                                for (int i = 0; i < pwrtArgument.length(); i++) {
                                    if (pwrtArgument[i] == ':') {
                                        indexTracker++;
                                    } else if (indexTracker == 0) {
                                        requestedCommand += pwrtArgument[i];
                                    } else if (indexTracker == 1) {
                                        requestedRow += pwrtArgument[i];
                                    } else {
                                        requestedColumn += pwrtArgument[i];
                                    }
                                }

                                targetTablet = std::tolower(requestedRow[0]);

                                // Pick up locks for write request and swap tablet if necessary.
                                pthread_mutex_lock(&activeTabletLock);
                                readWriteLock.lock_shared();
                                rwLockPickedUp = true;
                                if (activeTablet != targetTablet) {
                                    importTablet(targetTablet);
                                    activeTablet = targetTablet;
                                }

                                if (write(comm_FD, &putOkay[0], putOkay.length()) < 0) {
                                    fprintf(stderr, "PWRT failed to write: %s\n", strerror(errno));
                                }

                                int replicaGroup = 0;
                                if (myIndex == 1 || myIndex == 2 || myIndex == 3) {
                                    replicaGroup = 1;
                                } else if (myIndex == 4 || myIndex == 5 || myIndex == 6) {
                                    replicaGroup = 2;
                                } else if (myIndex == 7 || myIndex == 8 || myIndex == 9) {
                                    replicaGroup = 3;
                                }

                                fprintf(stderr, "[Current Primary: %s] Write completed for replica group #%d.\n", ipPorts[myIndex].data(), replicaGroup);
                            }  else if ((buf.size() > 7) &&
                                       (buf[0] == 'p' || buf[0] == 'P') &&
                                       (buf[1] == 'd' || buf[1] == 'D') &&
                                       (buf[2] == 'e' || buf[2] == 'E') &&
                                       (buf[3] == 'l' || buf[3] == 'L') &&
                                       (buf[4] == ':')) {
                                // Pseudo shutdown enabled. Reject request.
                                if (pseudoShutDown == true) {
                                    break;
                                }

                                // DELETE request received from secondary.
                                std::string pdelArgument = "";
                                requestedCommand = "DELE";
                                requestedRow = "";
                                requestedColumn = "";
                                int indexTracker = 0;
                                bool valueExists = true;
                                pdelArgument.append(buf.begin() + 5, buf.begin() + i);

                                // Parse the arguments.
                                for (int i = 0; i < pdelArgument.length(); i++) {
                                    if (pdelArgument[i] == ':') {
                                        indexTracker++;
                                    } else if (indexTracker == 0) {
                                        requestedRow += pdelArgument[i];
                                    } else if (indexTracker == 1) {
                                        requestedColumn += pdelArgument[i];
                                    }
                                }

                                targetTablet = std::tolower(requestedRow[0]);

                                // Pick up locks for delete request and swap tablet if necessary.
                                pthread_mutex_lock(&activeTabletLock);
                                readWriteLock.lock();
                                rwLockPickedUp = true;
                                if (activeTablet != targetTablet) {
                                    importTablet(targetTablet);
                                    activeTablet = targetTablet;
                                }

                                if (keyValueStore[requestedRow][requestedColumn] != nullptr) {
                                    // Deallocate memory and erase from KVS.
                                    sequenceNumber++;
                                    logActivity(sequenceNumber, targetTablet, "DELE", requestedRow, requestedColumn, "", "");
                                    delete keyValueStore[requestedRow][requestedColumn];
                                    keyValueStore[requestedRow].erase(requestedColumn);

                                    // Add nodes to list for parsing.
                                    std::string nodeList = "";
                                    for (int i = 0; i < activeNodes.size(); i++) {
                                        nodeList += activeNodes[i];
                                        if (i != activeNodes.size() - 1) {
                                            nodeList += ',';
                                        }
                                    }

                                    writeToGroup(nodeList, "DELE", requestedRow, requestedColumn, nullptr);

                                    int replicaGroup = 0;
                                    if (myIndex == 1 || myIndex == 2 || myIndex == 3) {
                                        replicaGroup = 1;
                                    } else if (myIndex == 4 || myIndex == 5 || myIndex == 6) {
                                        replicaGroup = 2;
                                    } else if (myIndex == 7 || myIndex == 8 || myIndex == 9) {
                                        replicaGroup = 3;
                                    }
                                    fprintf(stderr, "[Current Primary: %s] Delete completed for replica group #%d.\n", ipPorts[myIndex].data(), replicaGroup);
                                } else {
                                    valueExists = false;
                                }

                                if (valueExists == true) {
                                    if (write(comm_FD, &deleteOkay[0], deleteOkay.length()) < 0) {
                                        fprintf(stderr, "PDEL failed to write: %s\n", strerror(errno));
                                    }
                                } else {
                                    if (write(comm_FD, &invalidValue[0], invalidValue.length()) < 0) {
                                        fprintf(stderr, "PDEL failed to write: %s\n", strerror(errno));
                                    }
                                }

                                // Release locks.
                                if (rwLockPickedUp == true) {
                                    pthread_mutex_unlock(&activeTabletLock);
                                    readWriteLock.unlock();
                                    rwLockPickedUp = false;
                                } else {
                                    fprintf(stderr, "Lock logic incorrect PDEL.\n");
                                }
                            }  else if ((buf.size() > 7) &&
                                       (buf[0] == 'w' || buf[0] == 'W') &&
                                       (buf[1] == 'r' || buf[1] == 'R') &&
                                       (buf[2] == 'i' || buf[2] == 'I') &&
                                       (buf[3] == 't' || buf[3] == 'T') &&
                                       (buf[4] == ':')) {
                                // Pseudo shutdown enabled. Reject request.
                                if (pseudoShutDown == true) {
                                    break;
                                }
                                
                                // WRITE command received from primary.
                                std::string writArgument = "";
                                requestedCommand = "";
                                requestedRow = "";
                                requestedColumn = "";
                                std::string newSequenceNumber;
                                int indexTracker = 0;
                                writArgument.append(buf.begin() + 5, buf.begin() + i);
                                writeReceived = true;

                                // Parse the arguments.
                                for (int i = 0; i < writArgument.length(); i++) {
                                    if (writArgument[i] == ':') {
                                        indexTracker++;
                                    } else if (indexTracker == 0) {
                                        requestedCommand += writArgument[i];
                                    } else if (indexTracker == 1) {
                                        requestedRow += writArgument[i];
                                    } else if (indexTracker == 2) {
                                        requestedColumn += writArgument[i];
                                    } else {
                                        newSequenceNumber += writArgument[i];
                                    }
                                }

                                targetTablet = std::tolower(requestedRow[0]);
                                sequenceNumber = std::stoi(newSequenceNumber);

                                // Pick up locks for write request and swap tablet if necessary.
                                pthread_mutex_lock(&activeTabletLock);
                                readWriteLock.lock();
                                rwLockPickedUp = true;
                                if (activeTablet != targetTablet) {
                                    importTablet(targetTablet);
                                    activeTablet = targetTablet;
                                }

                                if (write(comm_FD, &putOkay[0], putOkay.length()) < 0) {
                                    fprintf(stderr, "WRIT failed to write: %s\n", strerror(errno));
                                }

                                int replicaGroup = 0;
                                if (myIndex == 1 || myIndex == 2 || myIndex == 3) {
                                    replicaGroup = 1;
                                } else if (myIndex == 4 || myIndex == 5 || myIndex == 6) {
                                    replicaGroup = 2;
                                } else if (myIndex == 7 || myIndex == 8 || myIndex == 9) {
                                    replicaGroup = 3;
                                }
                                fprintf(stderr, "[Replica: %s] Write completed at secondary node of replica group #%d.\n", ipPorts[myIndex].data(), replicaGroup);
                            }  else if ((buf.size() > 7) &&
                                       (buf[0] == 'r' || buf[0] == 'R') &&
                                       (buf[1] == 'e' || buf[1] == 'E') &&
                                       (buf[2] == 'm' || buf[2] == 'M') &&
                                       (buf[3] == 'v' || buf[3] == 'V') &&
                                       (buf[4] == ':')) {
                                // Pseudo shutdown enabled. Reject request.
                                if (pseudoShutDown == true) {
                                    break;
                                }

                                // DELETE command received from primary.
                                std::string remvArgument = "";
                                requestedCommand = "";
                                requestedRow = "";
                                requestedColumn = "";
                                std::string newSequenceNumber;
                                int indexTracker = 0;
                                remvArgument.append(buf.begin() + 5, buf.begin() + i);

                                // Parse the arguments.
                                for (int i = 0; i < remvArgument.length(); i++) {
                                    if (remvArgument[i] == ':') {
                                        indexTracker++;
                                    } else if (indexTracker == 0) {
                                        requestedRow += remvArgument[i];
                                    } else if (indexTracker == 1) {
                                        requestedColumn += remvArgument[i];
                                    } else {
                                        newSequenceNumber += remvArgument[i];
                                    }
                                }
                                targetTablet = std::tolower(requestedRow[0]);
                                sequenceNumber = std::stoi(newSequenceNumber);

                                // Pick up locks for delete request and swap tablet if necessary.
                                pthread_mutex_lock(&activeTabletLock);
                                readWriteLock.lock();
                                rwLockPickedUp = true;
                                if (activeTablet != targetTablet) {
                                    importTablet(targetTablet);
                                    activeTablet = targetTablet;
                                }

                                if (keyValueStore[requestedRow][requestedColumn] != nullptr) {
                                    // Deallocate memory and erase from KVS.
                                    logActivity(sequenceNumber, targetTablet, "DELE", requestedRow, requestedColumn, "", "");
                                    delete keyValueStore[requestedRow][requestedColumn];
                                    keyValueStore[requestedRow].erase(requestedColumn);

                                    fprintf(stderr, "[%d] DELETE successful at the following location:\n[%d] Row: %s\n[%d] Column: %s\n", 
                                            comm_FD, comm_FD, requestedRow.data(), comm_FD, requestedColumn.data());
                                }

                                if (write(comm_FD, &deleteOkay[0], deleteOkay.length()) < 0) {
                                    fprintf(stderr, "REMV failed to write: %s\n", strerror(errno));
                                }

                                int replicaGroup = 0;
                                if (myIndex == 1 || myIndex == 2 || myIndex == 3) {
                                    replicaGroup = 1;
                                } else if (myIndex == 4 || myIndex == 5 || myIndex == 6) {
                                    replicaGroup = 2;
                                } else if (myIndex == 7 || myIndex == 8 || myIndex == 9) {
                                    replicaGroup = 3;
                                }
                                fprintf(stderr, "[Replica: %s] Delete completed at secondary node of replica group #%d.\n", ipPorts[myIndex].data(), replicaGroup);

                                // Release locks.
                                if (rwLockPickedUp == true) {
                                    pthread_mutex_unlock(&activeTabletLock);
                                    readWriteLock.unlock();
                                    rwLockPickedUp = false;
                                } else {
                                    fprintf(stderr, "Lock logic incorrect REMV.\n");
                                }
                            }  else if ((buf.size() > 7) &&
                                       (buf[0] == 'p' || buf[0] == 'P') &&
                                       (buf[1] == 'r' || buf[1] == 'R') &&
                                       (buf[2] == 'i' || buf[2] == 'I') &&
                                       (buf[3] == 'm' || buf[3] == 'M') &&
                                       (buf[4] == ':')) {
                                // Pseudo shutdown enabled. Reject request.
                                if (pseudoShutDown == true) {
                                    break;
                                }

                                // Backend server down. Update primary status and list of active storage servers.
                                pthread_mutex_lock(&primaryUpdateLock);
                                if (buf[5] == '1') {
                                    currentPrimary = true;
                                    primaryAddress = ipPorts[myIndex];
                                    activeNodes.clear();

                                    // Parse the arguments.
                                    std::string primArgument = "";
                                    std::string nextValue = "";
                                    int indexTracker = 0;
                                    primArgument.append(buf.begin() + 7, buf.begin() + i);
                                    std::stringstream ss(primArgument);

                                    while (!ss.eof()) {
                                        std::getline(ss, nextValue, ',');
                                        activeNodes.push_back(nextValue);
                                    }

                                    pthread_mutex_unlock(&primaryUpdateLock);

                                    if (write(comm_FD, &primMessage[0], primMessage.length()) < 0) {
                                        fprintf(stderr, "PRIM failed to write: %s\n", strerror(errno));
                                    }

                                    fprintf(stderr, "[Current Primary: %s] Storage server statuses updated - currently active nodes: %s\n", 
                                            ipPorts[myIndex].data(), primArgument.data());
                                } else {
                                    currentPrimary = false;

                                    // Parse the arguments.
                                    std::string primArgument = "";
                                    primArgument.append(buf.begin() + 7, buf.begin() + i);
                                    primaryAddress = primArgument;

                                    pthread_mutex_unlock(&primaryUpdateLock);

                                    if (write(comm_FD, &primMessage[0], primMessage.length()) < 0) {
                                        fprintf(stderr, "PRIM failed to write: %s\n", strerror(errno));
                                    }

                                    fprintf(stderr, "[Replica: %s] Primary down - new primary: %s\n", ipPorts[myIndex].data(), primaryAddress.data());
                                }
                            } else if ((buf[0] == 's' || buf[0] == 'S') && 
                                    (buf[1] == 't' || buf[1] == 'T') && 
                                    (buf[2] == 'd' || buf[2] == 'D') &&
                                    (buf[3] == 'n' || buf[3] == 'N') &&
                                    (buf[4] == '\r') &&
                                    (buf[5] == '\n')) {
                                // Pseudo shutdown enabled. Reject request.
                                if (pseudoShutDown == true) {
                                    break;
                                }

                                pseudoShutDown = true;
                                currentPrimary = false;
                                activeTablet = '0';

                                // Deallocate KVS memory to simulate shutdown.
                                kvsCleanup();

                                // std::cerr << "STDN command received. Pseudo shutdown enabled." << std::endl;
                                fprintf(stderr,"[Storage Server: %s] Has died - waiting for recovery signal from master server", ipPorts[myIndex].c_str());

                                if (write(comm_FD, stdnResponse.c_str(), stdnResponse.length()) < 0) {
                                    fprintf(stderr, "STDN failed to write: %s\n", strerror(errno));
                                }

                                char* shutdownSignal = new char;
                                *shutdownSignal = 'X';

                                // Separate SIGINT from new output.
                                fprintf(stderr, "\n");

                                // Write the shutdown signal for threads to see.
                                write(shutDownPipe[1], shutdownSignal, 1);

                                delete shutdownSignal;
                            } else if ((buf[0] == 'r' || buf[0] == 'R') && 
                                    (buf[1] == 's' || buf[1] == 'S') && 
                                    (buf[2] == 't' || buf[2] == 'T') &&
                                    (buf[3] == 't' || buf[3] == 'T') &&
                                    (buf[4] == '\r') &&
                                    (buf[5] == '\n')) {   
                                if (pseudoShutDown == false) {
                                    // Invalid command received.
                                    if (write(comm_FD, &invalidSequenceOfCommands[0], invalidSequenceOfCommands.length()) < 0) {
                                        fprintf(stderr, "RSTT failed to write: %s\n", strerror(errno));
                                    }
                                } else {
                                    if (write(comm_FD, &rsttResponse[0], rsttResponse.length()) < 0) {
                                        fprintf(stderr, "RSTT failed to write: %s\n", strerror(errno));
                                    }
                                    recoveryMode = true;
                                    requestedCommand = "RECOVER";
                                    recovery();
                                    requestedCommand = "DEFAULT";
                                }
                            } else if ((buf.size() > 25) &&
                                    (buf[0] == 'u' || buf[0] == 'U') && 
                                    (buf[1] == 'p' || buf[1] == 'P') && 
                                    (buf[2] == 'd' || buf[2] == 'D') &&
                                    (buf[3] == 't' || buf[3] == 'T')) {
                                // Pseudo shutdown enabled. Reject request.
                                if (pseudoShutDown == true) {
                                    break;
                                }

                                pthread_mutex_lock(&activeTabletLock);
                                // Parse the arguments.
                                std::string updtArgument = "";
                                std::string nextValue = "";
                                std::string recoveringNodeAddress = "";
                                std::string response = "+OK ";
                                std::vector<std::string> updtArgs;
                                int characterTracker = 0;
                                updtArgument.append(buf.begin() + 5, buf.begin() + i);
                                std::stringstream ss(updtArgument);
                                std::unordered_map<char, std::string> receivedRecordCounts;

                                while (!ss.eof()) {
                                    std::getline(ss, nextValue, ':');
                                    // if (nextValue[nextValue.length() - 1] == '\n') {
                                    // nextValue = nextValue.substr(0, nextValue.length() - 1);
                                    // }
                                    updtArgs.push_back(nextValue);
                                }

                                recoveringNodeAddress += updtArgs[0] + ':' + updtArgs[1];

                                for (int i = 2; i < updtArgs.size() - 1; i++) {
                                    if (i % 2 == 0) {
                                        receivedRecordCounts[updtArgs[i][0]] = updtArgs[i + 1];
                                    }
                                }

                                // Get most recent checkpoint sequence numbers for each tablet.
                                loadRecordCounts();

                                // Get log file and determine its tablet.
                                std::vector<char> lastLog = loadLogFile();
                                char logTablet = '0';
                                if (lastLog.size() > 0) {
                                    // Log has transactions. Determine tablet.
                                    int argCounter = 0;
                                    for (int i = 0; i < lastLog.size(); i++) {
                                        if (lastLog[i] == ':') {
                                            argCounter++;
                                        } else if (argCounter == 2) {
                                            logTablet = lastLog[i];
                                            break;
                                        }
                                    }
                                } else {
                                    // All logs are empty.
                                }

                                for (auto tab : recordCounts) {
                                    response = response + tab.first + ':' + recordCounts[tab.first] + ':';
                                    if (std::stoi(recordCounts[tab.first]) == std::stoi(receivedRecordCounts[tab.first])) {
                                        // Version number of the previous tablet is the same. Send over log file for this tablet to recover missing entries.
                                        if (logTablet != '0') {
                                            sendLogFileData(recoveringNodeAddress, lastLog, logTablet);
                                        }
                                    } else if (std::stoi(recordCounts[tab.first]) > std::stoi(receivedRecordCounts[tab.first])) {
                                        // A checkpoint occurred while the recovering node was down. Send over the full tablet.
                                        std::vector<char> currentCheckpoint = loadCheckpointFile(tab.first);
                                        sendCheckpointFile(recoveringNodeAddress, currentCheckpoint, tab.first);
                                    }
                                }
                                response = response.substr(0, response.length() - 1);
                                response += "\r\n";

                                // Reply once all of the data has been sent to the recovering node.
                                if (write(comm_FD, &response[0], response.length()) < 0) {
                                    fprintf(stderr, "UPDT failed to write: %s\n", strerror(errno));
                                }

                                // Release locks.
                                pthread_mutex_unlock(&activeTabletLock);

                                // Reset any state values.
                            } else if ((buf[0] == 'f' || buf[0] == 'F') && 
                                    (buf[1] == 'i' || buf[1] == 'I') && 
                                    (buf[2] == 'l' || buf[2] == 'L') &&
                                    (buf[3] == 'e' || buf[3] == 'E')) {
                                if (pseudoShutDown == false) {
                                    // Invalid command received.
                                    if (write(comm_FD, &invalidSequenceOfCommands[0], invalidSequenceOfCommands.length()) < 0) {
                                        fprintf(stderr, "FILE failed to write: %s\n", strerror(errno));
                                    }
                                } else {
                                    // Parse the arguments.
                                    requestedCommand = "RECOVER";
                                    std::string fileArgument = "";
                                    fileArgument.append(buf.begin() + 5, buf.begin() + i);
                                    recoveringTablet = fileArgument[0];
                                    if (fileArgument[2] == '0') {
                                        logOrCheckpoint = false;
                                    } else if (fileArgument[2] == '1') {
                                        logOrCheckpoint = true;
                                    } else {
                                        fprintf(stderr, "Input error in FILE.\n");
                                    }

                                    if (write(comm_FD, &putOkay[0], putOkay.length()) < 0) {
                                        fprintf(stderr, "RSTT failed to write: %s\n", strerror(errno));
                                    }
                                }
                            } else if ((buf[0] == 'p' || buf[0] == 'P') && 
                                    (buf[1] == 'a' || buf[1] == 'A') && 
                                    (buf[2] == 'i' || buf[2] == 'I') &&
                                    (buf[3] == 'r' || buf[3] == 'R') &&
                                    (buf[4] == '\r') &&
                                    (buf[5] == '\n')) {
                                // Pseudo shutdown enabled. Reject request.
                                if (pseudoShutDown == true) {
                                    break;
                                }

                                // Return all row, column pairs in this replica group.
                                pthread_mutex_lock(&activeTabletLock);
                                readWriteLock.lock();
                                std::string response = "+OK ";

                                for (int i = 0; i < myTablets.size(); i++) {
                                    importTablet(myTablets[i]);

                                    for (auto row : keyValueStore) {
                                        for (auto col : keyValueStore[row.first]) {
                                            response += row.first + ',' + col.first + ':';
                                        }
                                    }
                                }

                                response = response.substr(0, response.length() - 1);

                                response += "\r\n";

                                pthread_mutex_unlock(&activeTabletLock);
                                readWriteLock.unlock();
                                if (write(comm_FD, &response[0], response.length()) < 0) {
                                    fprintf(stderr, "PAIR failed to write: %s\n", strerror(errno));
                                }
                            } else {
                                // Pseudo shutdown enabled. Reject request.
                                if (pseudoShutDown == true) {
                                    break;
                                }

                                // Invalid command received.
                                if (write(comm_FD, &invalidCommand[0], invalidCommand.length()) < 0) {
                                    fprintf(stderr, "Server failed to write: %s\n", strerror(errno));
                                }
                            }

                            // Remove the previous command from the buffer.
                            std::vector<char> tempArray;
                            tempArray.insert(tempArray.begin(), buf.begin(), buf.end());
                            buf.clear();

                            for (int j = i + 2; j < tempArray.size(); j++) {
                                buf.push_back(tempArray[j]);
                            }

                            // Done checking for command, continue accepting input.
                            break;
                        }
                    }
                }
            }
        }

        // Server shutdown enabled.
        if (pseudoShutDown == true && recoveryMode == false) {
            break;
        }

        continueReading = false;
    }

    if (serverShutDown == true || (pseudoShutDown == true && recoveryMode == false)) {
       // Server shutting down. Write message to client and close connection.
       write(comm_FD, &serverShutDownMessage[0], serverShutDownMessage.length());

       close(comm_FD);
    } else {
       if (clientDisconnected == false) {
           // Quit requested. Send farewell message and close this connection.
           write(comm_FD, &quitMessage[0], quitMessage.length());
       }
       // Close the connection and update active fileDescriptors.
       close(comm_FD);
    }

    // Update active thread information, clean up memory, and exit.
    pthread_mutex_lock(&threadUpdatesLock);
    closedConnections.push_back(static_cast<threadDetails*>(connectionInfo)->myIndex);
    pthread_mutex_unlock(&threadUpdatesLock);

    if (static_cast<threadDetails*>(connectionInfo) != nullptr) {
        delete static_cast<threadDetails*>(connectionInfo);
    }

    if (valueData != nullptr) {
        delete valueData;
    }

    pthread_exit(NULL);
}

void signalHandler(int signal) {
    serverShutDown = true;
    char* shutdownSignal = new char;
    *shutdownSignal = 'X';

    // Separate SIGINT from new output.
    fprintf(stderr, "\n");

    // Write the shutdown signal for threads to see.
    write(shutDownPipe[1], shutdownSignal, 1);

    delete shutdownSignal;
}

void kvsCleanup() {
    for (auto currentRow : keyValueStore) {
        for (auto currentColumn : keyValueStore[currentRow.first]) {
            if (keyValueStore[currentRow.first][currentColumn.first] != nullptr) {
                delete keyValueStore[currentRow.first][currentColumn.first];
            }
        }
    }
    keyValueStore.clear();
}

void logActivity(int seqNum, char tablet, std::string action, std::string row, std::string column, std::string value, std::string length) {
    std::string currentRecord = "";
    tablet = std::tolower(tablet);

    if (action == "PUT") {
        currentRecord = std::to_string(seqNum) + ':' +
                        action + ':' + 
                        row + ':' + 
                        column + ':' + 
                        length + '\n' + 
                        value;
    } else if (action == "DELE") {
        currentRecord = std::to_string(seqNum) + ':' +
                        action + ':' + 
                        row + ':' + 
                        column + '\n';
    }

    // Write log record to disk.
    char myDirectory[PATH_MAX];
    int currentFD;
    getcwd(myDirectory, sizeof(myDirectory));
    std::string logDirectory(myDirectory);
    logDirectory = logDirectory + '/' + "storage_node_" + std::to_string(myIndex) + "/activity_logs/" + "tablet_log_" + tablet;
    currentFD = open(logDirectory.data(), O_RDWR | O_CREAT | O_APPEND, S_IRUSR | S_IWUSR);
    flock(currentFD, LOCK_EX);

    if (write(currentFD, currentRecord.data(), currentRecord.length()) < 0) {
        fprintf(stderr, "Failed to log activity to disk: %s\n", strerror(errno));
    }

    // Release lock and close file.
    flock(currentFD, LOCK_UN);
    close(currentFD);

    // Write most recently recorded sequence number to disk.
    std::string seqDirectory(myDirectory);
    seqDirectory = seqDirectory + '/' + "storage_node_" + std::to_string(myIndex) + "/activity_logs/" + "sequence_number_" + tablet;
    currentFD = open(seqDirectory.data(), O_RDWR | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
    flock(currentFD, LOCK_EX);

    if (write(currentFD, std::to_string(seqNum).data(), std::to_string(seqNum).length()) < 0) {
        fprintf(stderr, "Failed to write activity log to disk: %s\n", strerror(errno));
    }

    // Release lock and close file.
    flock(currentFD, LOCK_UN);
    close(currentFD);
}

void checkpointUpdate() {
    char myDirectory[PATH_MAX];
    int currentFD;
    getcwd(myDirectory, sizeof(myDirectory));
    std::string directory(myDirectory);
    
    // Open and lock file.
    directory = directory + '/' + "storage_node_" + std::to_string(myIndex) + "/tablets/" + "tablet_" + activeTablet;
    currentFD = open(directory.data(), O_RDWR | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
    flock(currentFD, LOCK_EX);

    for (auto currentRow : keyValueStore) {
        for (auto currentColumn : keyValueStore[currentRow.first]) {
            if (keyValueStore[currentRow.first][currentColumn.first] != nullptr) {
                std::string header = currentRow.first + ':' + currentColumn.first + ':' + std::to_string(keyValueStore[currentRow.first][currentColumn.first]->length()) + '\n';

                // Write the header information to disk.
                if (write(currentFD, header.data(), header.length()) < 0) {
                    fprintf(stderr, "Failed to write checkpoint to disk: %s\n", strerror(errno));
                }

                // Write the value to disk.
                if (write(currentFD, keyValueStore[currentRow.first][currentColumn.first]->data(), keyValueStore[currentRow.first][currentColumn.first]->length()) < 0) {
                    fprintf(stderr, "Failed to write checkpoint to disk: %s\n", strerror(errno));
                }
            }
        }
    }

    // Release lock and close file.
    flock(currentFD, LOCK_UN);
    close(currentFD);

    // Clear the log file.
    std::string logDirectory(myDirectory);
    logDirectory = logDirectory + '/' + "storage_node_" + std::to_string(myIndex) + "/activity_logs/" + "tablet_log_" + activeTablet;
    currentFD = open(logDirectory.data(), O_RDWR | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
    close(currentFD);
}

void* diskUpdatesThread(void* threadInfo) {
    while (true) {
        sleep(1);
        if (pseudoShutDown == true) {
            // Server shut down received from admin console. Continue until recovery concludes.
            continue;
        }

        pthread_mutex_lock(&activeTabletLock);
        
        if (serverShutDown == true) {
            // CTRL+C requested.
            break;
        } else if (pseudoShutDown == true) {
            // Server shut down received from admin console. Continue until recovery concludes.
            pthread_mutex_unlock(&activeTabletLock);
            continue;
        }

        if (activeTablet != '0') {
            checkpointUpdate();
            
            if (serverShutDown == true) {
                // CTRL+C requested.
                break;
            }
        }
        
        pthread_mutex_unlock(&activeTabletLock);

        if (serverShutDown == true) {
            // CTRL+C requested.
            break;
        } else if (pseudoShutDown == true) {
            // Server shut down received from admin console. Continue until recovery concludes.
            continue;
        }

        // Periodically wait before performing the next checkpoint update.
        sleep(1);

        if (serverShutDown == true) {
            // CTRL+C requested.
            break;
        } else if (pseudoShutDown == true) {
            // Server shut down received from admin console. Continue until recovery concludes.
            continue;
        }
    }

    pthread_exit(NULL);
}

void importTablet(char tablet) {
    char lastTablet = activeTablet;

    if (lastTablet != '0') {
        // // Pick up lock for the last tablet.
        // kvsTabletLocks[lastTablet - 'a'].lock();

        // Perform checkpointing on last tablet.
        checkpointUpdate();

        // Eject the last tablet.
        kvsCleanup();
    } else {
        // This is the first tablet being imported.
        activeTablet = std::tolower(tablet);
    }

    std::vector<char> buf;
    char myDirectory[PATH_MAX];
    bool readingValue = false;
    bool endOfFile = false;
    int headerCount = 0;
    int currentFD;
    int currentSize = 0;
    int targetSize;
    int numRead;
    int lastIndex;
    std::string currentRow = "";
    std::string currentColumn = "";
    std::string currentValue = "";
    std::string rawTargetSize = "";
    std::string stringBuffer = "";
    getcwd(myDirectory, sizeof(myDirectory));
    std::string directory(myDirectory);

    // Open file and pick up file lock.
    directory = directory + '/' + "storage_node_" + std::to_string(myIndex) + "/tablets/" + "tablet_" + tablet;
    currentFD = open(directory.data(), O_RDWR | S_IRUSR | S_IWUSR);
    flock(currentFD, LOCK_EX);

    while (currentFD != -1) {
        std::vector<char> tempBuf(20000);  // Temporary buffer for reading data.

        // Value collection mode enabled.
        if (readingValue == true) {            
            // Gather value from the buffer.
            for (int i = 0; i < buf.size(); i++) {
                stringBuffer += buf[i];
                lastIndex = i + 1;
                
                if (stringBuffer.length() == targetSize) {
                    // Add string to (row, column).
                    readingValue = false;
                    keyValueStore[currentRow][currentColumn] = new std::string(stringBuffer);

                    currentRow = "";
                    currentColumn = "";
                    rawTargetSize = "";
                    break;
                }
            }

            // Final value added to KVS. Exit loop.
            if (endOfFile == true && lastIndex == buf.size()) {
                break;
            }

            // Remove the read contents from the buffer.
            std::vector<char> dataTempArray;
            dataTempArray.insert(dataTempArray.begin(), buf.begin(), buf.end());
            buf.clear();
            int dataBufPosition = 0;

            for (int j = lastIndex; j < dataTempArray.size(); j++) {
                buf.push_back(dataTempArray[j]);
            }
        }

        // Server shutdown enabled.
        if (serverShutDown == true) {
            break;
        }

        // Read in the data and store it in the temp buffer.
        numRead = read(currentFD, tempBuf.data(), 2000);

        if (numRead == 0) {
            // End of file reached.
            endOfFile = true;
        }

        // Copy contents of temp array into full array.
        buf.insert(buf.end(), tempBuf.begin(), tempBuf.begin() + numRead);

        // No contents to parse. Exit loop.
        if (buf.size() == 0) {
            break;
        }
        
        while (readingValue == false) {
            // Determine the next row, column, and value size.
            for (int i = 0; i < buf.size(); i++) {
                if (buf[i] == '\n') {
                    // Size and header fully extracted.
                    targetSize = std::stoi(rawTargetSize);
                    readingValue = true;
                    headerCount = 0;
                    stringBuffer = "";
                    lastIndex = i + 1;

                    // Remove the header from the buffer.
                    std::vector<char> dataTempArray;
                    dataTempArray.insert(dataTempArray.begin(), buf.begin(), buf.end());
                    buf.clear();
                    int dataBufPosition = 0;

                    for (int j = lastIndex; j < dataTempArray.size(); j++) {
                        buf.push_back(dataTempArray[j]);
                    }

                    break;
                } else if (buf[i] == ':' && headerCount < 2) {
                    headerCount++;
                } else if (headerCount == 0) {
                    // Extract the row.
                    currentRow += buf[i];
                } else if (headerCount == 1) {
                    // Extract the column.
                    currentColumn += buf[i];
                } else {
                    // Extract the value size.
                    rawTargetSize += buf[i];
                }
            }

            // Full header not extracted from file. Read and try again.
            if (readingValue == false) {
                headerCount = 0;
                currentRow = "";
                currentColumn = "";
                rawTargetSize = "";
                break;
            }
        }
    }

    // Release lock and close file.
    flock(currentFD, LOCK_UN);
    close(currentFD);

    activeTablet = std::tolower(tablet);
}

std::string requestPrimary() {
    std::string primary;
    int openFD;
    int numRead;
    int status;
    struct sockaddr_in address;
    std::vector<char> buf;  // Buffer to store server response.
    std::vector<char> tempBuf(20000);  // Stores reads and transfers values to buf.
    std::string command = "GTPM," + ipPorts[myIndex] + "\r\n";

    if ((openFD = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        fprintf(stderr, "Error opening socket in request primary\n");
    }

    address.sin_family = AF_INET;
    address.sin_port = htons(masterPort);

    if (inet_pton(AF_INET, masterIP.data(), &address.sin_addr) <= 0) {
        fprintf(stderr, "Invalid address in request primary\n");
    }

    if ((status = connect(openFD, (struct sockaddr*)&address, sizeof(address))) < 0) {
        fprintf(stderr, "Unable to connect in request primary\n");
    }

    // Get welcome message from server.
    while (true) {
        numRead = read(openFD, tempBuf.data(), 2000);

        if (numRead <= 0) {
            fprintf(stderr, "Server disconnected or error occurred in request primary\n");
        }

        // Transfer read data to buf.
        for (int i = 0; i < numRead; i++) {
            buf.push_back(tempBuf[i]);
        }

        // Check if full message was acquired.
        if (buf.size() == 36 && buf[0] == '+') {
            buf.clear();
            tempBuf.clear();
            break;
        } else {
            // Clear tempBuf and try again.
            tempBuf.clear();
        }
    }

    // Send command to server.
    write(openFD, &command[0], command.length());

    // Get primary response from server.
    while (true) {
        numRead = read(openFD, tempBuf.data(), 2000);

        if (numRead <= 0) {
            fprintf(stderr, "Server disconnected or error occurred in request primary\n");
        }

        // Transfer read data to buf.
        for (int i = 0; i < numRead; i++) {
            buf.push_back(tempBuf[i]);
        }

        // Check if full message was acquired.
        if (buf.size() > 4 && buf[buf.size() - 1] == '\n' && buf[0] == '+') {
            // Parse the response to get the primary's IP:Port.
            for (int i = 4; i < buf.size(); i++) {
                if (buf[i] == '\r') {
                    break;
                } else {
                    primary += buf[i];
                }
            }

            buf.clear();
            tempBuf.clear();
            break;
        } else if (buf.size() == 41 && buf[0] == '-') {
            buf.clear();
            tempBuf.clear();
            break;
            primary = "";
        } else {
            // Clear tempBuf and try again.
            tempBuf.clear();
        }
    }

    // Update primary for this node.
    primaryAddress = primary;

    return primary;
}

bool writeToPrimary(std::string primary, std::string action, std::string row, std::string column, std::string* data) {
    int openFD;
    int numRead;
    int status;
    bool err = false;
    struct sockaddr_in address;
    std::vector<char> buf;  // Buffer to store server response.
    std::vector<char> tempBuf(20000);  // Stores reads and transfers values to buf.
    std::string command = "";

    if (action == "PUT") {
        command = "PWRT:" + action + ':' + row + ':' + column + "\r\n";
    } else {
        command = "PDEL:" + row + ':' + column + "\r\n";
    }

    if ((openFD = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        fprintf(stderr, "Error opening socket in primary write\n");
    }

    // Parse primary's data.
    bool indexTracker = false;
    std::string ipAddr = "";
    std::string rawPort = "";
    int primaryPort;
    for (int i = 0; i < primary.length(); i++) {
        if (primary[i] == ':') {
            indexTracker = true;
        } else if (indexTracker == false) {
            // Get IP.
            ipAddr += primary[i];
        } else {
            rawPort += primary[i];
        }
    }

    primaryPort = std::stoi(rawPort);
    address.sin_family = AF_INET;
    address.sin_port = htons(primaryPort);

    if (inet_pton(AF_INET, ipAddr.data(), &address.sin_addr) <= 0) {
        fprintf(stderr, "Invalid address in primary write\n");
    }

    if ((status = connect(openFD, (struct sockaddr*)&address, sizeof(address))) < 0) {
        fprintf(stderr, "Unable to connect in primary write\n");
    }

    // Get welcome message from server.
    while (true) {
        numRead = read(openFD, tempBuf.data(), 2000);

        if (numRead <= 0) {
            fprintf(stderr, "Server disconnected or error occurred in primary write\n");
        }

        // Transfer read data to buf.
        for (int i = 0; i < numRead; i++) {
            buf.push_back(tempBuf[i]);
        }

        // Check if full message was acquired.
        if (buf.size() == 18 && buf[0] == '+') {
            buf.clear();
            tempBuf.clear();
            break;
        } else {
            // Clear tempBuf and try again.
            tempBuf.clear();
        }
    }

    // Send command to server.
    write(openFD, &command[0], command.length());

    // Get response from server.
    while (true) {
        numRead = read(openFD, tempBuf.data(), 2000);

        if (numRead <= 0) {
            fprintf(stderr, "Server disconnected or error occurred in primary write\n");
        }

        // Transfer read data to buf.
        for (int i = 0; i < numRead; i++) {
            buf.push_back(tempBuf[i]);
        }

        // Check if full message was acquired.
        if (action == "PUT" && buf.size() == 26 && buf[0] == '+') {
            buf.clear();
            tempBuf.clear();
            break;
        } else if (action == "DELE" && buf.size() == 19 && buf[0] == '+') {
            buf.clear();
            tempBuf.clear();
            break;
        } else if (action == "DELE" && buf.size() == 27 && buf[0] == '-') {
            buf.clear();
            tempBuf.clear();
            err = true;
            break;
        } else {
            // Clear tempBuf and try again.
            tempBuf.clear();
        }
    }

    if (action == "PUT") {
        // Send data to server.
        command = "DATA\r\n";
        write(openFD, &command[0], command.length());

        // Get response from server.
        while (true) {
            numRead = read(openFD, tempBuf.data(), 2000);

            if (numRead <= 0) {
                fprintf(stderr, "Server disconnected or error occurred in primary write\n");
            }

            // Transfer read data to buf.
            for (int i = 0; i < numRead; i++) {
                buf.push_back(tempBuf[i]);
            }

            // Check if full message was acquired.
            if (buf.size() == 43 && buf[0] == '+') {
                buf.clear();
                tempBuf.clear();
                break;
            } else {
                // Clear tempBuf and try again.
                tempBuf.clear();
            }
        }

        write(openFD, data->data(), data->length());
        command = "\r\n.\r\n";
        write(openFD, &command[0], command.length());

        // Get response from server.
        while (true) {
            numRead = read(openFD, tempBuf.data(), 2000);

            if (numRead <= 0) {
                fprintf(stderr, "Server disconnected or error occurred in primary write\n");
            }


            // Transfer read data to buf.
            for (int i = 0; i < numRead; i++) {
                buf.push_back(tempBuf[i]);
            }

            // Check if full message was acquired.
            if (buf.size() == 17 && buf[0] == '+') {
                buf.clear();
                tempBuf.clear();
                break;
            } else {
                // Clear tempBuf and try again.
                tempBuf.size();
            }
        }

        command = "QUIT\r\n";
        write(openFD, &command[0], command.length());

        // Get response and close connection.
        while (true) {
            numRead = read(openFD, tempBuf.data(), 2000);

            if (numRead <= 0) {
                fprintf(stderr, "Server disconnected or error occurred in primary write\n");
            }

            // Transfer read data to buf.
            for (int i = 0; i < numRead; i++) {
                buf.push_back(tempBuf[i]);
            }

            // Check if full message was acquired.
            if (buf.size() == 14 && buf[0] == '+') {
                buf.clear();
                tempBuf.clear();
                break;
            } else {
                // Clear tempBuf and try again.
                tempBuf.clear();
            }
        }
    } else {
        command = "QUIT\r\n";
        write(openFD, &command[0], command.length());

        // Get response and close connection.
        while (true) {
            numRead = read(openFD, tempBuf.data(), 2000);

            if (numRead <= 0) {
                fprintf(stderr, "Server disconnected or error occurred in primary write\n");
            }

            // Transfer read data to buf.
            for (int i = 0; i < numRead; i++) {
                buf.push_back(tempBuf[i]);
            }

            // Check if full message was acquired.
            if (buf.size() == 14 && buf[0] == '+') {
                buf.clear();
                tempBuf.clear();
                break;
            } else {
                // Clear tempBuf and try again.
                tempBuf.clear();
            }
        }
    }

    return false;
}

void writeToGroup(std::string activeNodes, std::string action, std::string row, std::string column, std::string* data) {
    std::string firstNode = "";
    std::string secondNode = "";
    std::string thirdNode = "";
    std::string firstIP = "", secondIP = "", thirdIP = "";
    std::string firstPort = "", secondPort = "", thirdPort = "";
    std::vector<int> nodePorts;
    int primaryIndex = 0;
    int indexTracker = 0;
    int ipTracker = 0;
    int nodeCount = 0;

    // Parse active node IPs and ports.
    for (int i = 0; i < activeNodes.length(); i++) {
        if (activeNodes[i] == ':') {
            ipTracker++;
        }

        if (activeNodes[i] == ',') {
            indexTracker++;
        } else if (indexTracker == 0) {
            if (ipTracker == 0) {
                firstIP += activeNodes[i];
            } else if (activeNodes[i] != ':') {
                firstPort += activeNodes[i];
            }

            firstNode += activeNodes[i];
        } else if (indexTracker == 1) {
            if (ipTracker == 1) {
                secondIP += activeNodes[i];
            } else if (activeNodes[i] != ':') {
                secondPort += activeNodes[i];
            }

            secondNode += activeNodes[i];
        } else {
            if (ipTracker == 2) {
                thirdIP += activeNodes[i];
            } else if (activeNodes[i] != ':') {
                thirdPort += activeNodes[i];
            }

            thirdNode += activeNodes[i];
        }
    }

    // Determine node count.
    if (thirdIP.length() > 0) {
        nodeCount = 3;
    } else if (secondIP.length() > 0) {
        nodeCount = 2;
    } else {
        nodeCount = 1;
    }

    // Primary is the only active node. Write unnecessary.
    if (nodeCount == 1) {
        return;
    }

    // Convert ports to integers.
    if (firstPort != "") {
        nodePorts.push_back(std::stoi(firstPort));
    }
    if (secondPort != "") {
        nodePorts.push_back(std::stoi(secondPort));
    }
    if (thirdPort != "") {
        nodePorts.push_back(std::stoi(thirdPort));
    }

    // Determine which node is the primary.
    if (firstNode == ipPorts[myIndex]) {
        primaryIndex = 0;
    } else if (secondNode == ipPorts[myIndex]) {
        primaryIndex = 1;
    } else if (thirdNode == ipPorts[myIndex]) {
        primaryIndex = 2;
    }

    for (int i = 0; i < nodePorts.size(); i++) {
        if (i == primaryIndex) {
            continue;
        }

        // Open a connection and write to each node that is not the primary.
        int openFD;
        int numRead;
        int status;
        int currentPort;
        std::string currentIP;
        int nodesContacted = 0;
        struct sockaddr_in address;
        std::vector<char> buf;  // Buffer to store server response.
        std::vector<char> tempBuf(20000);  // Stores reads and transfers values to buf.
        std::string command = "WRIT:" + action + ':' + row + ':' + column + ':' + std::to_string(sequenceNumber) + "\r\n";

        if (action == "PUT") {
            command = "WRIT:" + action + ':' + row + ':' + column + ':' + std::to_string(sequenceNumber) + "\r\n";
        } else {
            command = "REMV:" + row + ':' + column + ':' + std::to_string(sequenceNumber) + "\r\n";
        }

        // Get IP and port for this node.
        currentPort = nodePorts[i];
        if (i == 0) {
            currentIP = firstIP;
        } else if (i == 1) {
            currentIP = secondIP;
        } else {
            currentIP = thirdIP;
        }

        if ((openFD = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
            fprintf(stderr, "Error opening socket in write to group\n");
        }

        address.sin_family = AF_INET;
        address.sin_port = htons(currentPort);

        if (inet_pton(AF_INET, currentIP.data(), &address.sin_addr) <= 0) {
            fprintf(stderr, "Invalid address in write to group\n");
        }

        if ((status = connect(openFD, (struct sockaddr*)&address, sizeof(address))) < 0) {
            fprintf(stderr, "Unable to connect in write to group\n");
        }

        // Get welcome message from server.
        while (true) {
            numRead = read(openFD, tempBuf.data(), 2000);

            if (numRead <= 0) {
                fprintf(stderr, "Server disconnected or error occurred in write to group\n");
            }

            // Transfer read data to buf.
            for (int i = 0; i < numRead; i++) {
                buf.push_back(tempBuf[i]);
            }

            // Check if full message was acquired.
            if (buf.size() == 18 && buf[0] == '+') {
                buf.clear();
                tempBuf.clear();
                break;
            } else {
                // Clear tempBuf and try again.
                tempBuf.clear();
            }
        }

        // Send command to server.
        write(openFD, &command[0], command.length());

        // Get response from server.
        while (true) {
            numRead = read(openFD, tempBuf.data(), 2000);

            if (numRead <= 0) {
                fprintf(stderr, "Server disconnected or error occurred in write to group\n");
            }

            // Transfer read data to buf.
            for (int i = 0; i < numRead; i++) {
                buf.push_back(tempBuf[i]);
            }

            // Check if full message was acquired.
            if (action == "PUT" && buf.size() == 26 && buf[0] == '+') {
                // Full message acquired. Clear buffers.
                buf.clear();
                tempBuf.clear();
                break;
            } else if (action == "DELE" && buf.size() == 19 && buf[0] == '+') {
                // Full message acquired. Clear buffers.
                buf.clear();
                tempBuf.clear();
                break;
            } else {
                // Clear tempBuf and try again.
                tempBuf.clear();
            }
        }

        if (action == "PUT") {
            // Send data to server.
            command = "DATA\r\n";
            write(openFD, &command[0], command.length());

            // Get response from server.
            while (true) {
                numRead = read(openFD, tempBuf.data(), 2000);

                if (numRead <= 0) {
                    fprintf(stderr, "Server disconnected or error occurred in write to group\n");
                }

                // Transfer read data to buf.
                for (int i = 0; i < numRead; i++) {
                    buf.push_back(tempBuf[i]);
                }

                // Check if full message was acquired.
                if (buf.size() == 43 && buf[0] == '+') {
                    buf.clear();
                    tempBuf.clear();
                    break;
                } else {
                    // Clear tempBuf and try again.
                    tempBuf.clear();
                }
            }

            write(openFD, data->data(), data->length());
            command = "\r\n.\r\n";
            write(openFD, &command[0], command.length());

            // Get response from server.
            while (true) {
                numRead = read(openFD, tempBuf.data(), 2000);

                if (numRead <= 0) {
                    fprintf(stderr, "Server disconnected or error occurred in write to group\n");
                }

                // Transfer read data to buf.
                for (int i = 0; i < numRead; i++) {
                    buf.push_back(tempBuf[i]);
                }

                // Check if full message was acquired.
                if (buf.size() == 17 && buf[0] == '+') {
                    buf.clear();
                    tempBuf.clear();
                    break;
                } else {
                    // Clear tempBuf and try again.
                    tempBuf.clear();
                }
            }

            command = "QUIT\r\n";
            write(openFD, &command[0], command.length());

            // Get response and close connection.
            while (true) {
                numRead = read(openFD, tempBuf.data(), 2000);

                if (numRead <= 0) {
                    fprintf(stderr, "Server disconnected or error occurred in write to group\n");
                }

                // Transfer read data to buf.
                for (int i = 0; i < numRead; i++) {
                    buf.push_back(tempBuf[i]);
                }

                // Check if full message was acquired.
                if (buf.size() == 14 && buf[0] == '+') {
                    buf.clear();
                    tempBuf.clear();
                    break;
                } else {
                    // Clear tempBuf and try again.
                    tempBuf.clear();
                }
            }
        }  else {
            command = "QUIT\r\n";
            write(openFD, &command[0], command.length());

            // Get response and close connection.
            while (true) {
                numRead = read(openFD, tempBuf.data(), 2000);

                if (numRead <= 0) {
                    fprintf(stderr, "Server disconnected or error occurred in write to group\n");
                }

                // Transfer read data to buf.
                for (int i = 0; i < numRead; i++) {
                    buf.push_back(tempBuf[i]);
                }

                // Check if full message was acquired.
                if (buf.size() == 14 && buf[0] == '+') {
                    buf.clear();
                    tempBuf.clear();
                    break;
                } else {
                    // Clear tempBuf and try again.
                    tempBuf.clear();
                }
            }
        }
    }
}

void recovery() {
    // Get address of primary from master.
    std::string activePrimary = requestPrimary();

    if (activePrimary.length() > 0) {
        fprintf(stderr, "[Recovering Node: %s] Recovery initiated - contacting primary\n", ipPorts[myIndex].c_str());

        // Get the most recently checkpointed sequence numbers for each tablet.
        std::string argument = ipPorts[myIndex];

        // Get most recently checkpointed sequence numbers.
        loadRecordCounts();

        for (auto tab : recordCounts) {
            argument = argument + ':' + tab.first + ':' + recordCounts[tab.first];
        }

        // Load the most recent log that was not checkpointed.
        std::vector<char> mostRecentLog = loadLogFile();
        
        // Send recovery request to primary.
        requestData(activePrimary, argument);
    } else {
        fprintf(stderr, "[Recovering Node: %s] No nodes currently active - independent recovery initiated\n", ipPorts[myIndex].c_str());
    }

    // Notify master.
    recoveryComplete();

    // Reset state.
    lastLogFile = '0';
    recoveryMode = false;
    recordCounts.clear();
    pseudoShutDown = false;
    logOrCheckpoint = false;
    recoveringTablet = '0';

    fprintf(stderr, "[Recovering Node: %s] Recovery complete.\n", ipPorts[myIndex].c_str());
}

void loadRecordCounts() {
    std::vector<char> buf;
    char myDirectory[PATH_MAX];
    bool endOfFile = false;
    int currentFD;
    int numRead = 0;
    getcwd(myDirectory, sizeof(myDirectory));

    for (int i = 0; i < myTablets.length(); i++) {
        // Open tablet sequence number.
        std::string directory(myDirectory);
        directory = directory + '/' + "storage_node_" + std::to_string(myIndex) + "/activity_logs/" + "sequence_number_" + myTablets[i];
        currentFD = open(directory.data(), O_RDWR | S_IRUSR | S_IWUSR);

        while (currentFD != -1 && endOfFile == false) {
            std::vector<char> tempBuf(20000);  // Temporary buffer for reading data.

            // Server shutdown enabled.
            if (serverShutDown == true) {
                break;
            }

            // Read in the data and store it in the temp buffer.
            numRead = read(currentFD, tempBuf.data(), 2000);

            if (numRead == 0) {
                // End of file reached.
                endOfFile = true;
            }

            // Copy contents of temp array into full array.
            buf.insert(buf.end(), tempBuf.begin(), tempBuf.begin() + numRead);

            // No contents to parse. Exit loop.
            if (buf.size() == 0) {
                break;
            }
        }

        // Add the extracted value to recordCounts.
        if (buf.size() > 0) {
            std::string newCount;
            for (int k = 0; k < buf.size(); k++) {
                if (buf[k] != '\n') {
                    newCount.push_back(buf[k]);
                }
            }
            recordCounts[myTablets[i]] = newCount;
        } else {
            recordCounts[myTablets[i]] = "0";
        }

        buf.clear();
        endOfFile = false;
        close(currentFD);
    }
}

std::vector<char> loadLogFile() {
    std::vector<char> buf;
    char myDirectory[PATH_MAX];
    bool endOfFile = false;
    int currentFD;
    int numRead = 0;
    getcwd(myDirectory, sizeof(myDirectory));

    for (int i = 0; i < myTablets.length(); i++) {
        // Open tablet sequence number.
        std::string directory(myDirectory);
        directory = directory + '/' + "storage_node_" + std::to_string(myIndex) + "/activity_logs/" + "tablet_log_" + myTablets[i];
        currentFD = open(directory.data(), O_RDWR | S_IRUSR | S_IWUSR);

        while (currentFD != -1 && endOfFile == false) {
            std::vector<char> tempBuf(20000);  // Temporary buffer for reading data.

            // Server shutdown enabled.
            if (serverShutDown == true) {
                break;
            }

            // Read in the data and store it in the temp buffer.
            numRead = read(currentFD, tempBuf.data(), 2000);

            if (numRead == 0) {
                // End of file reached.
                endOfFile = true;
            }

            // Copy contents of temp array into full array.
            buf.insert(buf.end(), tempBuf.begin(), tempBuf.begin() + numRead);

            // No contents to parse. Exit loop.
            if (buf.size() == 0) {
                break;
            }

            if (endOfFile == true && buf.size() > 0) {
                lastLogFile = myTablets[i];
                break;
            }
        }
    }

    close(currentFD);

    return buf;
}

std::vector<char> loadCheckpointFile(char tablet) {
    std::vector<char> buf;
    char myDirectory[PATH_MAX];
    bool endOfFile = false;
    int currentFD;
    int numRead = 0;
    getcwd(myDirectory, sizeof(myDirectory));
    std::tolower(tablet);

    // Open tablet sequence number.
    std::string directory(myDirectory);
    directory = directory + '/' + "storage_node_" + std::to_string(myIndex) + "/tablets/" + "tablet_" + tablet;
    currentFD = open(directory.data(), O_RDWR | S_IRUSR | S_IWUSR);

    while (currentFD != -1 && endOfFile == false) {
        std::vector<char> tempBuf(20000);  // Temporary buffer for reading data.

        // Server shutdown enabled.
        if (serverShutDown == true) {
            break;
        }

        // Read in the data and store it in the temp buffer.
        numRead = read(currentFD, tempBuf.data(), 2000);

        if (numRead == 0) {
            // End of file reached.
            endOfFile = true;
        }

        // Copy contents of temp array into full array.
        buf.insert(buf.end(), tempBuf.begin(), tempBuf.begin() + numRead);

        // No contents to parse. Exit loop.
        if (buf.size() == 0) {
            break;
        }

        if (endOfFile == true && buf.size() > 0) {
            break;
        }
    }

    close(currentFD);

    return buf;
}

void saveLogFile(char tablet, std::string* data) {
    char myDirectory[PATH_MAX];
    int currentFD;
    getcwd(myDirectory, sizeof(myDirectory));
    std::string directory(myDirectory);
    tablet = std::tolower(tablet);
    
    // Open and lock file.
    directory = directory + '/' + "storage_node_" + std::to_string(myIndex) + "/activity_logs/" + "tablet_" + tablet;
    currentFD = open(directory.data(), O_RDWR | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
    flock(currentFD, LOCK_EX);

    // Write the header information to disk.
    if (write(currentFD, data->data(), data->length()) < 0) {
        fprintf(stderr, "Failed to write log to disk in saveLogFile: %s\n", strerror(errno));
    }

    // Release lock and close file.
    flock(currentFD, LOCK_UN);
    close(currentFD);
}

void saveCheckpointFile(char tablet, std::string* data) {
    char myDirectory[PATH_MAX];
    int currentFD;
    getcwd(myDirectory, sizeof(myDirectory));
    std::string directory(myDirectory);
    tablet = std::tolower(tablet);
    
    // Open and lock file.
    directory = directory + '/' + "storage_node_" + std::to_string(myIndex) + "/tablets/" + "tablet_" + tablet;
    currentFD = open(directory.data(), O_RDWR | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
    flock(currentFD, LOCK_EX);

    // Write the header information to disk.
    if (write(currentFD, data->data(), data->length()) < 0) {
        fprintf(stderr, "Failed to write log to disk in saveLogFile: %s\n", strerror(errno));
    }

    // Release lock and close file.
    flock(currentFD, LOCK_UN);
    close(currentFD);
}

void logFileRecovery() {

}

void sendLogFileData(std::string node, std::vector<char>& data, char tablet) {
    // std::string primary = ipPorts[primaryIndex];
    int openFD;
    int numRead;
    int status;
    struct sockaddr_in address;
    std::string nodeIP = "";
    std::string nodePort = "";
    std::vector<char> buf;  // Buffer to store server response.
    std::vector<char> tempBuf(20000);  // Stores reads and transfers values to buf.
    std::string command = "";
    bool ipTracker = false;
    tablet = std::tolower(tablet);
    command = command + "FILE:" + tablet + ":0\r\n";

    // Separate IP and port from node's address.
    for (int i = 0; i < node.length(); i++) {
        if (node[i] == ':') {
            ipTracker = true;
        } else if (ipTracker == true) {
            nodePort += node[i];
        } else {
            nodeIP += node[i];
        }
    }

    if ((openFD = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        fprintf(stderr, "Error opening socket in sendLogFile\n");
    }

    address.sin_family = AF_INET;
    address.sin_port = htons(std::stoi(nodePort));

    if (inet_pton(AF_INET, nodeIP.data(), &address.sin_addr) <= 0) {
        fprintf(stderr, "Invalid address in sendLogFile\n");
    }

    if ((status = connect(openFD, (struct sockaddr*)&address, sizeof(address))) < 0) {
        fprintf(stderr, "Unable to connect in sendLogFile\n");
    }

    // Get welcome message from server.
    while (true) {
        numRead = read(openFD, tempBuf.data(), 2000);

        if (numRead <= 0) {
            fprintf(stderr, "Server disconnected or error occurred in sendLogFile\n");
        }

        // Transfer read data to buf.
        for (int i = 0; i < numRead; i++) {
            buf.push_back(tempBuf[i]);
        }

        // Check if full message was acquired.
        if (buf.size() == 18 && buf[0] == '+') {
            buf.clear();
            tempBuf.clear();
            break;
        } else {
            // Clear tempBuf and try again.
            tempBuf.clear();
        }
    }

    // Send command to server.
    write(openFD, &command[0], command.length());

    // Get response from server.
    while (true) {
        numRead = read(openFD, tempBuf.data(), 2000);

        // Transfer read data to buf.
        for (int i = 0; i < numRead; i++) {
            buf.push_back(tempBuf[i]);
        }

        // Check if full message was acquired.
        if (buf.size() == 26 && buf[0] == '+') {
            buf.clear();
            tempBuf.clear();
            break;
        } else {
            // Clear tempBuf and try again.
            tempBuf.clear();
        }
    }

    // Send data to server.
    command = "DATA\r\n";
    write(openFD, &command[0], command.length());

    // Get response from server.
    while (true) {
        numRead = read(openFD, tempBuf.data(), 2000);

        if (numRead <= 0) {
            fprintf(stderr, "Server disconnected or error occurred in primary write\n");
        }

        // Transfer read data to buf.
        for (int i = 0; i < numRead; i++) {
            buf.push_back(tempBuf[i]);
        }

        // Check if full message was acquired.
        if (buf.size() == 43 && buf[0] == '+') {
            buf.clear();
            tempBuf.clear();
            break;
        } else {
            // Clear tempBuf and try again.
            tempBuf.clear();
        }
    }

    write(openFD, data.data(), data.size());
    command = "\r\n.\r\n";
    write(openFD, &command[0], command.length());

    // Get response from server.
    while (true) {
        numRead = read(openFD, tempBuf.data(), 2000);

        if (numRead <= 0) {
            fprintf(stderr, "Server disconnected or error occurred in primary write\n");
        }

        // Transfer read data to buf.
        for (int i = 0; i < numRead; i++) {
            buf.push_back(tempBuf[i]);
        }

        // Check if full message was acquired.
        if (buf.size() == 17 && buf[0] == '+') {
            buf.clear();
            tempBuf.clear();
            break;
        } else {
            // Clear tempBuf and try again.
            tempBuf.size();
        }
    }

    command = "QUIT\r\n";
    write(openFD, &command[0], command.length());

    // Get response and close connection.
    while (true) {
        numRead = read(openFD, tempBuf.data(), 2000);

        if (numRead <= 0) {
            fprintf(stderr, "Server disconnected or error occurred in primary write\n");
        }

        // Transfer read data to buf.
        for (int i = 0; i < numRead; i++) {
            buf.push_back(tempBuf[i]);
        }

        // Check if full message was acquired.
        if (buf.size() == 14 && buf[0] == '+') {
            buf.clear();
            tempBuf.clear();
            break;
        } else {
            // Clear tempBuf and try again.
            tempBuf.clear();
        }
    }

    close(openFD);
}

void sendCheckpointFile(std::string node, std::vector<char>& data, char tablet) {
    int openFD;
    int numRead;
    int status;
    struct sockaddr_in address;
    std::string nodeIP = "";
    std::string nodePort = "";
    std::vector<char> buf;  // Buffer to store server response.
    std::vector<char> tempBuf(20000);  // Stores reads and transfers values to buf.
    std::string command = "";
    bool ipTracker = false;
    tablet = std::tolower(tablet);
    command = command + "FILE:" + tablet + ":1\r\n";

    // Separate IP and port from node's address.
    for (int i = 0; i < node.length(); i++) {
        if (node[i] == ':') {
            ipTracker = true;
        } else if (ipTracker == true) {
            nodePort += node[i];
        } else {
            nodeIP += node[i];
        }
    }

    if ((openFD = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        fprintf(stderr, "Error opening socket in sendLogFile\n");
    }

    address.sin_family = AF_INET;
    address.sin_port = htons(std::stoi(nodePort));

    if (inet_pton(AF_INET, nodeIP.data(), &address.sin_addr) <= 0) {
        fprintf(stderr, "Invalid address in sendLogFile\n");
    }

    if ((status = connect(openFD, (struct sockaddr*)&address, sizeof(address))) < 0) {
        fprintf(stderr, "Unable to connect in sendLogFile\n");
    }

    // Get welcome message from server.
    while (true) {
        numRead = read(openFD, tempBuf.data(), 2000);

        if (numRead <= 0) {
            fprintf(stderr, "Server disconnected or error occurred in sendLogFile\n");
        }

        // Transfer read data to buf.
        for (int i = 0; i < numRead; i++) {
            buf.push_back(tempBuf[i]);
        }

        // Check if full message was acquired.
        if (buf.size() == 18 && buf[0] == '+') {
            buf.clear();
            tempBuf.clear();
            break;
        } else {
            // Clear tempBuf and try again.
            tempBuf.clear();
        }
    }

    // Send command to server.
    write(openFD, &command[0], command.length());

    // Get response from server.
    while (true) {
        numRead = read(openFD, tempBuf.data(), 2000);

        // Transfer read data to buf.
        for (int i = 0; i < numRead; i++) {
            buf.push_back(tempBuf[i]);
        }

        // Check if full message was acquired.
        if (buf.size() == 26 && buf[0] == '+') {
            buf.clear();
            tempBuf.clear();
            break;
        } else {
            // Clear tempBuf and try again.
            tempBuf.clear();
        }
    }

    // Send data to server.
    command = "DATA\r\n";
    write(openFD, &command[0], command.length());

    // Get response from server.
    while (true) {
        numRead = read(openFD, tempBuf.data(), 2000);

        if (numRead <= 0) {
            fprintf(stderr, "Server disconnected or error occurred in primary write\n");
        }

        // Transfer read data to buf.
        for (int i = 0; i < numRead; i++) {
            buf.push_back(tempBuf[i]);
        }

        // Check if full message was acquired.
        if (buf.size() == 43 && buf[0] == '+') {
            buf.clear();
            tempBuf.clear();
            break;
        } else {
            // Clear tempBuf and try again.
            tempBuf.clear();
        }
    }

    write(openFD, data.data(), data.size());
    command = "\r\n.\r\n";
    write(openFD, &command[0], command.length());

    // Get response from server.
    while (true) {
        numRead = read(openFD, tempBuf.data(), 2000);

        if (numRead <= 0) {
            fprintf(stderr, "Server disconnected or error occurred in primary write\n");
        }

        // Transfer read data to buf.
        for (int i = 0; i < numRead; i++) {
            buf.push_back(tempBuf[i]);
        }

        // Check if full message was acquired.
        if (buf.size() == 17 && buf[0] == '+') {
            buf.clear();
            tempBuf.clear();
            break;
        } else {
            // Clear tempBuf and try again.
            tempBuf.size();
        }
    }

    command = "QUIT\r\n";
    write(openFD, &command[0], command.length());

    // Get response and close connection.
    while (true) {
        numRead = read(openFD, tempBuf.data(), 2000);

        if (numRead <= 0) {
            fprintf(stderr, "Server disconnected or error occurred in primary write\n");
        }

        // Transfer read data to buf.
        for (int i = 0; i < numRead; i++) {
            buf.push_back(tempBuf[i]);
        }

        // Check if full message was acquired.
        if (buf.size() == 14 && buf[0] == '+') {
            buf.clear();
            tempBuf.clear();
            break;
        } else {
            // Clear tempBuf and try again.
            tempBuf.clear();
        }
    }

    close(openFD);
}

void requestData(std::string primary, std::string updtArgument) {
    int openFD;
    int numRead;
    int status;
    struct sockaddr_in address;
    std::vector<char> buf;  // Buffer to store server response.
    std::vector<char> tempBuf(20000);  // Stores reads and transfers values to buf.
    std::string command = "UPDT:" + updtArgument + "\r\n";
    std::string primaryIP;
    std::string primaryPort;
    bool ipTracker = false;

    for (int i = 0; i < primary.length(); i++) {
        if (primary[i] == ':') {
            ipTracker = true;
        } else if (ipTracker == true) {
            primaryPort += primary[i];
        } else {
            primaryIP += primary[i];
        }
    }

    if ((openFD = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        fprintf(stderr, "Error opening socket in request primary\n");
    }

    address.sin_family = AF_INET;
    address.sin_port = htons(std::stoi(primaryPort));

    if (inet_pton(AF_INET, primaryIP.data(), &address.sin_addr) <= 0) {
        fprintf(stderr, "Invalid address in request primary\n");
    }

    if ((status = connect(openFD, (struct sockaddr*)&address, sizeof(address))) < 0) {
        fprintf(stderr, "Unable to connect in request primary\n");
    }

    // Get welcome message from server.
    while (true) {
        numRead = read(openFD, tempBuf.data(), 2000);

        if (numRead <= 0) {
            fprintf(stderr, "Server disconnected or error occurred in request primary\n");
        }

        // Transfer read data to buf.
        for (int i = 0; i < numRead; i++) {
            buf.push_back(tempBuf[i]);
        }

        // Check if full message was acquired.
        if (buf.size() == 18 && buf[0] == '+') {
            buf.clear();
            tempBuf.clear();
            break;
        } else {
            // Clear tempBuf and try again.
            tempBuf.clear();
        }
    }

    // Send command to server.
    write(openFD, &command[0], command.length());

    // Get response from server.
    while (true) {
        numRead = read(openFD, tempBuf.data(), 2000);

        if (numRead <= 0) {
            fprintf(stderr, "Server disconnected or error occurred in request primary\n");
        }

        // Transfer read data to buf.
        for (int i = 0; i < numRead; i++) {
            buf.push_back(tempBuf[i]);
        }

        // Check if full message was acquired.
        if (buf[buf.size() - 1] == '\n' && buf[0] == '+') {
            // Done. Update version numbers and wait for recovery requests in other threads.
            updateSequenceNumbers(buf);

            buf.clear();
            tempBuf.clear();
            break;
        } else {
            // Clear tempBuf and try again.
            tempBuf.clear();
        }
    }

    close(openFD);
}

void recoveryComplete() {
    int openFD;
    int numRead;
    int status;
    struct sockaddr_in address;
    std::vector<char> buf;  // Buffer to store server response.
    std::vector<char> tempBuf(20000);  // Stores reads and transfers values to buf.
    std::string command = "RCVY," + ipPorts[myIndex] + "\r\n";

    if ((openFD = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        fprintf(stderr, "Error opening socket in request primary\n");
    }

    address.sin_family = AF_INET;
    address.sin_port = htons(masterPort);

    if (inet_pton(AF_INET, masterIP.data(), &address.sin_addr) <= 0) {
        fprintf(stderr, "Invalid address in request primary\n");
    }

    if ((status = connect(openFD, (struct sockaddr*)&address, sizeof(address))) < 0) {
        fprintf(stderr, "Unable to connect in request primary\n");
    }

    // Get welcome message from server.
    while (true) {
        numRead = read(openFD, tempBuf.data(), 2000);

        if (numRead <= 0) {
            fprintf(stderr, "Server disconnected or error occurred in request primary\n");
        }

        // Transfer read data to buf.
        for (int i = 0; i < numRead; i++) {
            buf.push_back(tempBuf[i]);
        }

        // Check if full message was acquired.
        if (buf.size() == 36 && buf[0] == '+') {
            buf.clear();
            tempBuf.clear();
            break;
        } else {
            // Clear tempBuf and try again.
            tempBuf.clear();
        }
    }

    // Send command to server.
    write(openFD, &command[0], command.length());
    // Get response from server.
    while (true) {
        numRead = read(openFD, tempBuf.data(), 2000);

        if (numRead <= 0) {
            fprintf(stderr, "Server disconnected or error occurred in request primary\n");
        }

        // Transfer read data to buf.
        for (int i = 0; i < numRead; i++) {
            buf.push_back(tempBuf[i]);
        }

        // Check if full message was acquired.
        if (buf.size() == 26 && buf[buf.size() - 1] == '\n' && buf[0] == '+') {
            buf.clear();
            tempBuf.clear();
            break;
        } else {
            // Clear tempBuf and try again.
            tempBuf.clear();
        }
    }

    close(openFD);
}

void updateSequenceNumbers(std::vector<char> primaryResponse) {
    std::unordered_map<char, std::string> sequenceNumbers;
    std::string filteredArg(primaryResponse.begin() + 4, primaryResponse.end() - 2);
    std::stringstream ss(filteredArg);
    std::string nextValue = "";
    std::vector<std::string> argValues;
    char lastChar;
    int highestSequenceNumber = 0;

    // Parse argument.
    while (!ss.eof()) {
        std::getline(ss, nextValue, ':');
        if (nextValue[nextValue.length() - 1] == '\n') {
            nextValue = nextValue.substr(0, nextValue.length() - 1);
        }

        argValues.push_back(nextValue);
    }

    // Add arguments to map.
    for (int i = 0; i < argValues.size(); i++) {
        if ((i % 2) == 0) {
            // Character key.
            lastChar = std::tolower(argValues[i][0]);
        } else {
            // Sequence number. Update map and highest sequence number.
            sequenceNumbers[lastChar] = argValues[i];

            if (std::stoi(argValues[i]) > highestSequenceNumber) {
                highestSequenceNumber = std::stoi(argValues[i]);
            }
        }
    }

    sequenceNumber = highestSequenceNumber;

    for (auto tab : sequenceNumbers) {
        // Update disk sequence numbers.
        char myDirectory[PATH_MAX];
        int currentFD;
        std::string currentRecord = sequenceNumbers[tab.first];
        getcwd(myDirectory, sizeof(myDirectory));
        std::string logDirectory(myDirectory);
        logDirectory = logDirectory + '/' + "storage_node_" + std::to_string(myIndex) + "/activity_logs/" + "sequence_number_" + tab.first;
        currentFD = open(logDirectory.data(), O_RDWR | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
        flock(currentFD, LOCK_EX);

        if (write(currentFD, currentRecord.data(), currentRecord.length()) < 0) {
            fprintf(stderr, "Failed to log activity to disk: %s\n", strerror(errno));
        }

        // Release lock and close file.
        flock(currentFD, LOCK_UN);
        close(currentFD);
    }
}