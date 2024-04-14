#include "backendserver.h"

bool pFlag = false, vFlag = false;  // Command line argument flags.
int portNumber = 10000;
int myIndex;  // Stores the index of this node within the addresses list.

std::unordered_map<int, std::unordered_map<std::string, std::unordered_map<std::string, std::string*>>> keyValueStore; // Key-value store for this node's rows and backup storage.
std::unordered_map<int, std::string> activityLogs; // Activity record logs for this node and backup storage.

std::string serverGreeting = "+OK Server ready\r\n";
std::string invalidCommand = "-ERR Unknown command\r\n";
std::string invalidArgument = "-ERR Invalid argument\r\n";
std::string invalidSequenceOfCommands = "-ERR Invalid sequence of commands\r\n";
std::string quitMessage = "+OK Goodbye!\r\n";
std::string serverShutDownMessage = "-ERR Server shutting down\r\n";
std::string noFileExtension = "-ERR File extension not specified\r\n";
std::string putOkay = "+OK Send value with DATA\r\n";
std::string okayMessage = "+OK\r\n";
std::string invalidValue = "-ERR Value does not exist\r\n";
std::string cputOkay = "+OK Send first value with DATA\r\n";
std::string deleteOkay = "+OK Value deleted\r\n";
std::string valueAdded = "+OK Value added\r\n";
std::string dataOkay = "+OK Enter value ending with <CRLF>.<CRLF>\r\n";
std::string secondValueOkay = "+OK Enter second value with DATA\r\n";
std::string firstValueInvalid = "-ERR Value does not equal current value\r\n";

std::vector<int> availableThreadIndices;  // Thread indices currently available for creation.
std::vector<int> closedConnections;  // Stores thread indices associated with recently closed connections.
pthread_t activeThreads[100];  // Stores active threads. Only accessed by main thread.

bool serverShutDown = false;  // Tracks Ctrl+C command from user.
int shutDownPipe[2];  // Pipe for communicating shutdown status to threads.
bool shutDownCleanup = false;  // Signals when thread resources have been cleaned up.

pthread_mutex_t threadUpdatesLock;  // Lock for accessing availableThreadIndices and closedConnections.

// Main entry point for this program. See echoserver.h for function documentation.
// @returns  Exit code 0 for success, else error.
int main(int argc, char *argv[]) {
    // Parse arguments and set flags.
    parseArgs(argc, argv);

    // Watch for user's Ctrl+C signal.
    struct sigaction sigAction;
    sigAction.sa_handler = signalHandler;
    sigemptyset(&sigAction.sa_mask);
    sigAction.sa_flags = 0;
    sigaction(SIGINT, & sigAction, 0);

    // Establish connections and dispatch threads to handle them.
    connectionManager();

    return 0;
}

int parseArgs(int argc, char *argv[]) {
    int optValue = 0;

    // No command line options enabled.
    if (argc == 1) {
        return 0;
    }

    // Check for specified options.
    while ((optValue = getopt(argc, argv, "p:vi:")) != -1) {
        if (optValue == 'p') {
            // Port option enabled.
            pFlag = true;
            std::string newPort = optarg;
            portNumber = std::stoi(newPort);
        } else if (optValue == 'v') {
            // -v option enabled.
            vFlag = true;
        } else if (optValue == 'i') {
            // Set node index identifier.
            std::string newIndex = optarg;
            myIndex = std::stoi(newIndex);
        }
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
                // New connection debugger output.
                if (vFlag == true) {
                    fprintf(stderr, "[%d] New connection\n", nextThreadDetails->connectionFD);
                }

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

                    // Debugger output, thread joining information.
                    if (vFlag == true) {
                        fprintf(stderr, "[Normal Cleanup] Join on thread number:  %d\n", closedConnections[i]);
                        fprintf(stderr, "[Normal Cleanup] Available threads:  %lu\n", availableThreadIndices.size());
                    }
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

                // Debugger output, thread joining information.
                if (vFlag == true) {
                    fprintf(stderr, "[Shutdown Cleanup] Join on thread number:  %d\n", closedConnections[i]);
                    fprintf(stderr, "[Shutdown Cleanup] Available threads:  %lu\n", availableThreadIndices.size());
                }
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

    // Close file descriptors.
    close(listen_FD);
    close(shutDownPipe[0]);
    close(shutDownPipe[1]);

    // Deallocate KVS memory.
    for (const auto& currentIndex : keyValueStore) {
        for (const auto& currentRow : keyValueStore[currentIndex.first]) {
            for (const auto& currentColumn : keyValueStore[currentIndex.first][currentRow.first]) {
                if (keyValueStore[currentIndex.first][currentRow.first][currentColumn.first] != nullptr) {
                    delete keyValueStore[currentIndex.first][currentRow.first][currentColumn.first];
                }
            }
        }
    }
}

void* workerThread(void* connectionInfo) {
    char buf[2000];  // Communication buffer.
    char fileBuf[2000];  // File buffer.
    int comm_FD = static_cast<threadDetails*>(connectionInfo)->connectionFD;
    int numRead = 0;  // The number of bytes returned by read().
    int numWrite = 0; // The number of bytes written by write();
    bool quitCalled = false;
    bool continueReading = false;
    bool clientDisconnected = false;
    bool dataCalled = false;  // Tracks value input mode.
    bool cputSecondPass = false;  // Tracks whether or not v_1 is equal to the existing value.
    std::string requestedCommand = "Default";  // The client's most recently requested command.
    std::string requestedRow = "";
    std::string requestedColumn = "";
    std::string fileExtension = "";
    std::string* valueData = new std::string("");  // Stores the value sent via the DATA command.

    // Initialize buffers to null values.
    memset(buf, '\0', sizeof(buf));
    memset(fileBuf, '\0', sizeof(fileBuf));

    // Write greeting to client.
    write(comm_FD, &serverGreeting[0], serverGreeting.length());

    // Debugger output - greeting message.
    if (vFlag == true) {
        fprintf(stderr, "[%d] S: %s", comm_FD, serverGreeting.c_str());
    }

    // Maintain connection with client and execute commands.
    while (quitCalled == false) {
        // Read user input into intermediate array.
        char tempBuf[100];
        fd_set fdSet;  // Used to keep track of both client input and the server shutdown signal.

        FD_ZERO(&fdSet);
        FD_SET(comm_FD, &fdSet);
        FD_SET(shutDownPipe[0], &fdSet);

        // DATA processing mode.
        if (dataCalled == true && strlen(buf) > 0) {
            bool terminate = false;
            std::string dataToAdd = "";
            int lastIndex = 0;

            // Perform pass over data to look for terminating values.
            for (int i = 0; i < strlen(buf); i++) {
                if (buf[i] == '\r' &&
                    strlen(buf) - 1 - i >= 4 &&
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
                if (strlen(buf) >= 1500) {
                    // Offload 1000 characters.
                    dataToAdd.append(buf, 1000);
                    *valueData += dataToAdd;

                    // Remove characters from buffer that were added to valueData.
                    char dataTempArray[2000];
                    strcpy(dataTempArray, buf);
                    memset(buf, '\0', sizeof(buf));
                    int dataBufPosition = 0;

                    for (int j = 1000; j < strlen(dataTempArray); j++) {
                        buf[dataBufPosition++] = dataTempArray[j];
                    }
                }
            } else {
                // DATA content complete. Gather last string.
                dataToAdd.append(buf, lastIndex - 4);
                *valueData += dataToAdd;

                if (requestedCommand == "CPUT" && cputSecondPass == false) {
                    // Compare v_1 to existing value.
                    if (keyValueStore[myIndex][requestedRow][requestedColumn] != nullptr && *valueData == *keyValueStore[myIndex][requestedRow][requestedColumn]) {
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
                    }
                } else {
                    // Add value to KVS and update state.
                    if (write(comm_FD, &valueAdded[0], valueAdded.length()) < 0) {
                        fprintf(stderr, "DATA failed to write: %s\n", strerror(errno));
                    }

                    if (keyValueStore[myIndex][requestedRow][requestedColumn] != nullptr) {
                        delete keyValueStore[myIndex][requestedRow][requestedColumn];
                    }

                    keyValueStore[myIndex][requestedRow][requestedColumn] = valueData;
                    valueData = new std::string("");
                    requestedCommand = "Default";
                    cputSecondPass = false;
                }

                // Clean buffer.
                char dataTempArray[2000];
                strcpy(dataTempArray, buf);
                memset(buf, '\0', sizeof(buf));
                int dataBufPosition = 0;

                if (lastIndex < 1999) {
                    for (int j = lastIndex + 1; j < strlen(dataTempArray); j++) {
                        buf[dataBufPosition++] = dataTempArray[j];
                    }
                }

                // Exit DATA processing.
                dataCalled = false;

                if (vFlag == true) {
                    fprintf(stderr, "[%d] PUT/CPUT successful at the following location:\n[%d] Row: %s\n[%d] Column: %s\n[%d] Value: %s\n", 
                            comm_FD, comm_FD, requestedRow.c_str(), comm_FD, requestedColumn.c_str(), comm_FD, keyValueStore[myIndex][requestedRow][requestedColumn]->c_str());
                }
            }
        }

        // When shutdown pipe read available, enter shutdown mode.
        int returnValue = select(comm_FD + 1, &fdSet, NULL, NULL, NULL);

        // Server shutdown enabled.
        if (serverShutDown == true) {
            break;
        }

        numRead = read(comm_FD, tempBuf, 99);

        // Client has disconnected without requesting QUIT or read() failed. Terminate thread.
        if (numRead <= 0) {
            clientDisconnected = true;
            break;
        }

        tempBuf[numRead] = '\0';
        
        // Copy contents of temp array into full array.
        strncpy(buf + strlen(buf), tempBuf, numRead);

        // Check for complete commands in buffer. Exit loop when no commands remain.
        while (continueReading == false && quitCalled == false && dataCalled == false) {
            for (int i = 0; i < strlen(buf) - 1; i++) {
                // Last possible command in buffer. Continue reading after loop breaks.
                if (i == strlen(buf) - 2) {
                    continueReading = true;
                }

                if (buf[i] == '\r') {
                    if (i < strlen(buf) - 1) {
                        if (buf[i + 1] == '\n') {
                            // String is complete. Check for command.

                            if (strlen(buf) < 6) {
                                // Invalid command received.
                                if (write(comm_FD, &invalidCommand[0], invalidCommand.length()) < 0) {
                                    fprintf(stderr, "Failed to write: %s\n", strerror(errno));
                                }

                                // Debugger output - invalid command.
                                if (vFlag == true) {
                                    std::string currentCommand = "";
                                    currentCommand.append(buf, i + 2);
                                    fprintf(stderr, "[%d] C: %s", comm_FD, currentCommand.c_str());
                                    fprintf(stderr, "[%d] S: %s", comm_FD, invalidCommand.c_str());
                                }
                            } else if (strlen(buf) > 5 &&
                                    (buf[0] == 'q' || buf[0] == 'Q') && 
                                    (buf[1] == 'u' || buf[1] == 'U') && 
                                    (buf[2] == 'i' || buf[2] == 'I') &&
                                    (buf[3] == 't' || buf[3] == 'T') &&
                                    (buf[4] == '\r') &&
                                    (buf[5] == '\n')) {
                                // QUIT called, break from loops.
                                quitCalled = true;

                                // Debugger output - QUIT command.
                                if (vFlag == true) {
                                    std::string currentCommand = "";
                                    currentCommand.append(buf, i + 2);
                                    fprintf(stderr, "[%d] C: %s", comm_FD, currentCommand.c_str());
                                }

                                break;
                            } else if ((buf[0] == 'd' || buf[0] == 'D') && 
                                    (buf[1] == 'a' || buf[1] == 'A') && 
                                    (buf[2] == 't' || buf[2] == 'T') &&
                                    (buf[3] == 'a' || buf[3] == 'A') &&
                                    (buf[4] == '\r') &&
                                    (buf[5] == '\n')) {
                                // DATA called, break loop and enter email parsing mode.
                                if (requestedCommand == "PUT" || requestedCommand == "CPUT") {
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
                            } else if ((strlen(buf) > 8) &&
                                       (buf[0] == 'p' || buf[0] == 'P') &&
                                       (buf[1] == 'u' || buf[1] == 'U') &&
                                       (buf[2] == 't' || buf[2] == 'T') &&
                                       (buf[3] == ':')) {
                                // PUT() called. Extract the argument and test for validity.
                                std::string putArgument = "";
                                bool colTracker = false, extensionTracker = false;
                                putArgument.append(buf + 4, i - 3);
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
                                        if (putArgument[k] == '.') {
                                            extensionTracker = true;
                                        }
                                    } else if (putArgument[k] = ':') {
                                        colTracker = true;
                                    }
                                }

                                if (requestedRow.length() == 0 || requestedColumn.length() == 0 || extensionTracker == false) {
                                    // Invalid argument entered. Notify client.
                                    if (write(comm_FD, &invalidArgument[0], invalidArgument.length()) < 0) {
                                        fprintf(stderr, "PUT failed to write: %s\n", strerror(errno));
                                    }
                                } else {
                                    // Argument valid. Update state and notify client.
                                    requestedCommand = "PUT";
                                    if (write(comm_FD, &putOkay[0], putOkay.length()) < 0) {
                                        fprintf(stderr, "PUT failed to write: %s\n", strerror(errno));
                                    }
                                }
                            } else if ((strlen(buf) > 8) &&
                                       (buf[0] == 'g' || buf[0] == 'G') &&
                                       (buf[1] == 'e' || buf[1] == 'E') &&
                                       (buf[2] == 't' || buf[2] == 'T') &&
                                       (buf[3] == ':')) {
                                // GET() called. Extract the argument and test for validity.
                                std::string getArgument = "";
                                bool colTracker = false, extensionTracker = false;
                                getArgument.append(buf + 4, i - 3);
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
                                        if (getArgument[k] == '.') {
                                            extensionTracker = true;
                                        }
                                    } else if (getArgument[k] = ':') {
                                        colTracker = true;
                                    }
                                }

                                if (requestedRow.length() == 0 || requestedColumn.length() == 0 || extensionTracker == false) {
                                    if (write(comm_FD, &invalidArgument[0], invalidArgument.length()) < 0) {
                                        fprintf(stderr, "GET failed to write: %s\n", strerror(errno));
                                    }
                                } else if (keyValueStore[myIndex][requestedRow][requestedColumn] != nullptr) {
                                    // Send the requested value to user.
                                    std::string endOutput = "\r\n.\r\n";
                                    if (write(comm_FD, &okayMessage[0], okayMessage.length()) < 0) {
                                        fprintf(stderr, "GET failed to write: %s\n", strerror(errno));
                                    }
                                    if (write(comm_FD, keyValueStore[myIndex][requestedRow][requestedColumn]->c_str(), keyValueStore[myIndex][requestedRow][requestedColumn]->length()) < 0) {
                                        fprintf(stderr, "GET failed to write: %s\n", strerror(errno));
                                    }
                                    if (write(comm_FD, &endOutput[0], endOutput.length()) < 0) {
                                        fprintf(stderr, "GET failed to write: %s\n", strerror(errno));
                                    }

                                    if (vFlag == true) {
                                        fprintf(stderr, "[%d] GET successful at the following location:\n[%d] Row: %s\n[%d] Column: %s\n[%d] Value: %s\n", 
                                                comm_FD, comm_FD, requestedRow.c_str(), comm_FD, requestedColumn.c_str(), comm_FD, keyValueStore[myIndex][requestedRow][requestedColumn]->c_str());
                                    }
                                } else {
                                    if (write(comm_FD, &invalidValue[0], invalidValue.length()) < 0) {
                                        fprintf(stderr, "GET failed to write: %s\n", strerror(errno));
                                    }
                                }
                            } else if ((strlen(buf) > 9) &&
                                       (buf[0] == 'c' || buf[0] == 'C') &&
                                       (buf[1] == 'p' || buf[1] == 'P') &&
                                       (buf[2] == 'u' || buf[2] == 'U') &&
                                       (buf[3] == 't' || buf[3] == 'T') &&
                                       (buf[4] == ':')) {
                                // CPUT() called. Extract the argument and test for validity.
                                std::string cputArgument = "";
                                bool colTracker = false, extensionTracker = false;
                                cputArgument.append(buf + 5, i - 3);
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
                                        if (cputArgument[k] == '.') {
                                            extensionTracker = true;
                                        }
                                    } else if (cputArgument[k] = ':') {
                                        colTracker = true;
                                    }
                                }

                                if (requestedRow.length() == 0 || requestedColumn.length() == 0 || extensionTracker == false) {
                                    // Invalid argument.
                                    if (write(comm_FD, &invalidArgument[0], invalidArgument.length()) < 0) {
                                        fprintf(stderr, "CPUT failed to write: %s\n", strerror(errno));
                                    }
                                } else {
                                    // Argument valid, update state and notify user.
                                    requestedCommand = "CPUT";
                                    if (write(comm_FD, &cputOkay[0], cputOkay.length()) < 0) {
                                        fprintf(stderr, "CPUT failed to write: %s\n", strerror(errno));
                                    }
                                }
                            } else if ((strlen(buf) > 9) &&
                                       (buf[0] == 'd' || buf[0] == 'D') &&
                                       (buf[1] == 'e' || buf[1] == 'E') &&
                                       (buf[2] == 'l' || buf[2] == 'L') &&
                                       (buf[3] == 'e' || buf[3] == 'E') &&
                                       (buf[4] == ':')) {
                                // DELETE() called. Extract the argument and test for validity.
                                std::string deleArgument = "";
                                bool colTracker = false, extensionTracker = false;
                                deleArgument.append(buf + 5, i - 3);
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
                                        if (deleArgument[k] == '.') {
                                            extensionTracker = true;
                                        }
                                    } else if (deleArgument[k] = ':') {
                                        colTracker = true;
                                    }
                                }

                                if (requestedRow.length() == 0 || requestedColumn.length() == 0 || extensionTracker == false) {
                                    // Invalid argument.
                                    if (write(comm_FD, &invalidArgument[0], invalidArgument.length()) < 0) {
                                        fprintf(stderr, "DELE failed to write: %s\n", strerror(errno));
                                    }
                                } else {
                                    // Argument accepted, delete value and notify client.
                                    if (write(comm_FD, &deleteOkay[0], deleteOkay.length()) < 0) {
                                        fprintf(stderr, "DELE failed to write: %s\n", strerror(errno));
                                    }
                                    
                                    // Deallocate memory and erase from KVS.
                                    if (keyValueStore[myIndex][requestedRow][requestedColumn] != nullptr) {
                                        delete keyValueStore[myIndex][requestedRow][requestedColumn];
                                    }
                                    keyValueStore[myIndex][requestedRow].erase(requestedColumn);

                                    if (vFlag == true) {
                                        fprintf(stderr, "[%d] DELETE successful at the following location:\n[%d] Row: %s\n[%d] Column: %s\n", 
                                                comm_FD, comm_FD, requestedRow.c_str(), comm_FD, requestedColumn.c_str());
                                    }
                                }
                            } else {
                                // Invalid command received.
                                if (write(comm_FD, &invalidCommand[0], invalidCommand.length()) < 0) {
                                    fprintf(stderr, "Server failed to write: %s\n", strerror(errno));
                                }

                                // Debugger output - invalid command.
                                if (vFlag == true) {
                                    std::string currentCommand = "";
                                    currentCommand.append(buf, i + 2);
                                    fprintf(stderr, "[%d] C: %s", comm_FD, currentCommand.c_str());
                                    fprintf(stderr, "[%d] S: %s", comm_FD, invalidCommand.c_str());
                                }
                            }

                            // Remove the previous command from the buffer.
                            char tempArray[2000];
                            strcpy(tempArray, buf);
                            memset(buf, '\0', sizeof(buf));
                            int bufPosition = 0;

                            for (int j = i + 2; j < strlen(tempArray); j++) {
                                buf[bufPosition++] = tempArray[j];
                            }

                            // Done checking for command, continue accepting input.
                            break;
                        }
                    }
                }
            }
        }

        continueReading = false;
    }

    if (serverShutDown == false) {
        if (clientDisconnected == false) {
            // Quit requested. Send farewell message and close this connection.
            write(comm_FD, &quitMessage[0], quitMessage.length());

            // Debugger output - farewell message.
            if (vFlag == true) {
                fprintf(stderr, "[%d] S: %s", comm_FD, quitMessage.c_str());
            }
        }

        // Close the connection and update active fileDescriptors.
        close(comm_FD);

        // Debugger output - connection closed.
        if (vFlag == true) {
            fprintf(stderr, "[%d] Connection closed\n", comm_FD);
        }
    } else {
        // Server shutting down. Write message to client and close connection.
        write(comm_FD, &serverShutDownMessage[0], serverShutDownMessage.length());

        // Debugger output - server shutdown enabled and connection closed.
        if (vFlag == true) {
            fprintf(stderr, "[%d] S: %s", comm_FD, serverShutDownMessage.c_str());
            fprintf(stderr, "[%d] Connection closed\n", comm_FD);
        }

        close(comm_FD);
    }

    // Update active thread information, clean up memory, and exit.
    pthread_mutex_lock(&threadUpdatesLock);
    closedConnections.push_back(static_cast<threadDetails*>(connectionInfo)->myIndex);
    pthread_mutex_unlock(&threadUpdatesLock);

    delete static_cast<threadDetails*>(connectionInfo);
    delete valueData;

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