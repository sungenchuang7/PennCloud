#include "smtp.h"
#include "routes.h"
#include <openssl/md5.h>

bool pFlag = false, aFlag = false, vFlag = false; // Command line argument flags.
int portNumber = 2500;
std::string mailDirectory;                                   // The directory of our mailboxes.
std::vector<std::string> mailFileNames;                      // The file names for every mailbox in the directory.
std::vector<std::string> emailNames;                         // The recipent email names available for RCPT TO.
std::unordered_map<std::string, std::string> emailToMailbox; // Mapping from email to mailbox.

std::string serverGreeting = "220 localhost Simple Mail Transfer Service Ready\r\n";
std::string invalidCommand = "500 Syntax error, command unrecognized\r\n";
std::string quitMessage = "221 localhost Service closing transmission channel\r\n";
std::string heloResponse = "250 localhost\r\n";
std::string okayResponse = "250 OK\r\n";
std::string dataInputResponse = "354 Start mail input; end with <CRLF>.<CRLF>\r\n";
std::string badArgumentError = "501 Syntax error in parameters or arguments\r\n";
std::string badSequenceOfCommands = "503 Bad sequence of commands\r\n";
std::string badMailbox = "550 Requested action not taken: mailbox unavailable\r\n";

std::vector<int> availableThreadIndices; // Thread indices currently available for creation.
std::vector<int> closedConnections;      // Stores thread indices associated with recently closed connections.
pthread_t activeThreads[100];            // Stores active threads. Only accessed by main thread.

bool serverShutDown = false;  // Tracks Ctrl+C command from user.
int shutDownPipe[2];          // Pipe for communicating shutdown status to threads.
bool shutDownCleanup = false; // Signals when thread resources have been cleaned up.

pthread_mutex_t threadUpdatesLock; // Lock for accessing availableThreadIndices and closedConnections.
pthread_mutex_t *mailboxLocks;     // Stores a lock for each mailbox.

// Main entry point for this program. See smtp.h for function documentation.
// @returns  Exit code 0 for success, else error.
int main(int argc, char *argv[])
{
    // Parse arguments and set flags.
    parseArgs(argc, argv);

    if (aFlag == true)
    {
        fprintf(stderr, "Team 13\n");

        if (emailNames.size() > 0)
        {
            delete[] mailboxLocks;
        }

        return 0;
    }

    // Initialize locks.
    pthread_mutex_init(&threadUpdatesLock, NULL);
    for (int i = 0; i < emailNames.size(); i++)
    {
        pthread_mutex_init(&mailboxLocks[i], NULL);
    }

    // Watch for user's Ctrl+C signal.
    struct sigaction sigAction;
    sigAction.sa_handler = signalHandler;
    sigemptyset(&sigAction.sa_mask);
    sigAction.sa_flags = 0;
    sigaction(SIGINT, &sigAction, 0);

    // Establish connections and dispatch threads to handle them.
    connectionManager();

    return 0;
}

int parseArgs(int argc, char *argv[])
{
    int optValue = 0;
    DIR *directory;
    struct dirent *ent;

    // No command line options enabled.
    if (argc == 1)
    {
        return 0;
    }

    // Check for specified options.
    while ((optValue = getopt(argc, argv, "p:av")) != -1)
    {
        if (optValue == 'p')
        {
            // Port option enabled.
            pFlag = true;
            std::string newPort = optarg;
            portNumber = std::stoi(newPort);
        }
        else if (optValue == 'a')
        {
            // -a option enabled.
            aFlag = true;
        }
        else if (optValue == 'v')
        {
            // -v option enabled.
            vFlag = true;
        }
    }

    // Update directory name and existing file names.
    // mailDirectory = argv[optind];
    // directory = opendir(mailDirectory.c_str());

    // if (directory) {
    //     while ((ent = readdir(directory)) != NULL) {
    //         int nameLength = strlen(ent->d_name);
    //         // Check for proper mailbox format.
    //         if (nameLength >= 5) {
    //             if (ent->d_name[nameLength - 5] == '.' &&
    //                 ent->d_name[nameLength - 4] == 'm' &&
    //                 ent->d_name[nameLength - 3] == 'b' &&
    //                 ent->d_name[nameLength - 2] == 'o' &&
    //                 ent->d_name[nameLength - 1] == 'x') {
    //                     mailFileNames.push_back(ent->d_name);
    //             }
    //         }
    //     }
    //     closedir(directory);
    // }

    // Parse mailboxes, update email recipients list, and create email-mailbox mapping.
    for (int i = 0; i < mailFileNames.size(); i++)
    {
        std::string currentEmail = "";
        currentEmail = mailFileNames[i].substr(0, mailFileNames[i].length() - 5);
        currentEmail = currentEmail + "@localhost";
        emailNames.push_back(currentEmail);

        emailToMailbox[currentEmail] = mailFileNames[i];
    }

    // Create array of locks associated with each mailbox.
    if (emailNames.size() > 0)
    {
        mailboxLocks = new pthread_mutex_t[emailNames.size()];
    }

    return 0;
}

void connectionManager()
{
    // Create a new socket.
    int listen_FD = socket(PF_INET, SOCK_STREAM, 0);
    struct sockaddr_in servaddr;
    bzero(&servaddr, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htons(INADDR_ANY);
    servaddr.sin_port = htons(portNumber);
    struct timeval timeTracker; // Wait time before thread cleanup.
    fd_set fdSet;

    // Set up shutdown pipes.
    pipe(shutDownPipe);

    // Set up accept() timer.
    timeTracker.tv_sec = 2;
    timeTracker.tv_usec = 0;

    if (listen_FD < 0)
    {
        fprintf(stderr, "Fail to open socket: %s\n", strerror(errno));
        exit(1);
    }

    if (bind(listen_FD, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0)
    {
        fprintf(stderr, "Fail to bind: %s\n", strerror(errno));
        exit(1);
    }

    // Connect to the server.
    if (listen(listen_FD, 100) < 0)
    {
        fprintf(stderr, "Fail to listen: %s\n", strerror(errno));
        exit(1);
    }

    // Make all thread indices available, initialize fileDescriptors.
    for (int i = 0; i < 100; i++)
    {
        availableThreadIndices.push_back(i);
    }

    // Loop until server is shut down.
    while (serverShutDown == false)
    {
        // If a thread is available, dispatch it, else wait for a thread to be available.
        if (availableThreadIndices.size() > 0)
        {
            // Connect to the server and set up thread details.
            struct sockaddr_in clientaddr;
            socklen_t clientaddrlen = sizeof(clientaddr);
            threadDetails *nextThreadDetails = new threadDetails;
            nextThreadDetails->connectionFD = -1;

            FD_ZERO(&fdSet);
            FD_SET(listen_FD, &fdSet);
            int returnValue = select(listen_FD + 1, &fdSet, NULL, NULL, &timeTracker);

            if (returnValue > 0)
            {
                // Connection is available, accept and set thread details.
                nextThreadDetails->connectionFD = accept(listen_FD, (struct sockaddr *)&clientaddr, &clientaddrlen);
                nextThreadDetails->myIndex = availableThreadIndices.front();
                availableThreadIndices.erase(availableThreadIndices.begin());
            }

            // Create new thread with new connection.
            if (serverShutDown == false && nextThreadDetails->connectionFD != -1)
            {
                // New connection debugger output.
                if (vFlag == true)
                {
                    fprintf(stderr, "[%d] New connection\n", nextThreadDetails->connectionFD);
                }

                // Dispatch thread to handle this connection.
                pthread_create(&activeThreads[nextThreadDetails->myIndex], NULL, workerThread, nextThreadDetails);
            }
            else
            {
                // Don't create thread, continue to clean up.
                delete nextThreadDetails;
            }
        }

        // Handle closed connections.
        if (serverShutDown == false)
        {
            pthread_mutex_lock(&threadUpdatesLock);
            if (closedConnections.size() > 0)
            {
                // Join threads with closed connections.
                for (int i = 0; i < closedConnections.size(); i++)
                {
                    if (pthread_join(activeThreads[closedConnections[i]], nullptr) != 0)
                    {
                        std::cerr << "pthread_join failed." << std::endl;
                    }
                    availableThreadIndices.push_back(closedConnections[i]);

                    // Debugger output, thread joining information.
                    if (vFlag == true)
                    {
                        fprintf(stderr, "[Normal Cleanup] Join on thread number:  %d\n", closedConnections[i]);
                        fprintf(stderr, "[Normal Cleanup] Available threads:  %lu\n", availableThreadIndices.size());
                    }
                }

                // Empty closedConnections vector.
                closedConnections.clear();

                pthread_mutex_unlock(&threadUpdatesLock);
            }
            else
            {
                pthread_mutex_unlock(&threadUpdatesLock);
            }
        }
    }

    // Server shutdown enabled. Wait and join all threads.
    while (shutDownCleanup == false)
    {
        pthread_mutex_lock(&threadUpdatesLock);
        if (closedConnections.size() > 0)
        {
            // Join threads with closed connections.
            for (int i = 0; i < closedConnections.size(); i++)
            {
                if (pthread_join(activeThreads[closedConnections[i]], nullptr) != 0)
                {
                    std::cerr << "pthread_join failed." << std::endl;
                }
                availableThreadIndices.push_back(closedConnections[i]);

                // Debugger output, thread joining information.
                if (vFlag == true)
                {
                    fprintf(stderr, "[Shutdown Cleanup] Join on thread number:  %d\n", closedConnections[i]);
                    fprintf(stderr, "[Shutdown Cleanup] Available threads:  %lu\n", availableThreadIndices.size());
                }
            }

            // Empty closedConnections vector.
            closedConnections.clear();

            pthread_mutex_unlock(&threadUpdatesLock);
        }
        else
        {
            pthread_mutex_unlock(&threadUpdatesLock);
        }

        // Thread cleanup finished. Update signal.
        if (availableThreadIndices.size() == 100)
        {
            shutDownCleanup = true;
        }
    }

    // Close file descriptors and clean up memory.
    close(listen_FD);
    close(shutDownPipe[0]);
    close(shutDownPipe[1]);

    if (emailNames.size() > 0)
    {
        delete[] mailboxLocks;
    }

    pthread_mutex_destroy(&threadUpdatesLock);
}

void *workerThread(void *connectionInfo)
{
    char buf[2000]; // Buffer for reading data.
    int comm_FD = static_cast<threadDetails *>(connectionInfo)->connectionFD;
    int numRead = 0;  // The number of bytes returned by read().
    int numWrite = 0; // The number of bytes written by write();
    bool quitCalled = false;
    bool continueReading = false;
    bool clientDisconnected = false;
    bool dataCalled = false;                  // Tracks email data parsing mode.
    bool invalidCmd = true;                   // Tracks invalid command checking for MAIL FROM and RCPT TO commands.
    bool invalidArg = false;                  // Tracks invalid argument checking for MAIL FROM and RCPT TO commands.
    std::string mailFromAddress = "";         // Stores the sender of the email.
    std::vector<std::string> mailToAddresses; // Stores the receiver of the email.
    std::string currentState = "NoHelo";      // Stores the last received command from the user or initial state.
    std::vector<std::string> emailData;       // Stores the email received following a DATA command.

    // Initialize buf to null values.
    memset(buf, '\0', sizeof(buf));

    // Write greeting to client.
    write(comm_FD, &serverGreeting[0], serverGreeting.length());

    // Debugger output - greeting message.
    if (vFlag == true)
    {
        fprintf(stderr, "[%d] R: %s", comm_FD, serverGreeting.c_str());
    }

    // Maintain connection with client and execute commands.
    while (quitCalled == false)
    {
        // Read user input into intermediate array.
        char tempBuf[100];
        fd_set fdSet; // Used to keep track of both client input and the server shutdown signal.

        FD_ZERO(&fdSet);
        FD_SET(comm_FD, &fdSet);
        FD_SET(shutDownPipe[0], &fdSet);

        // DATA processing mode.
        if (dataCalled == true && strlen(buf) > 0)
        {
            bool terminate = false;
            std::string dataToAdd = "";
            int lastIndex = 0;

            // Perform pass over data to look for terminating values.
            for (int i = 0; i < strlen(buf); i++)
            {
                if (buf[i] == '\r' &&
                    strlen(buf) - 1 - i >= 4 &&
                    buf[i + 1] == '\n' &&
                    buf[i + 2] == '.' &&
                    buf[i + 3] == '\r' &&
                    buf[i + 4] == '\n')
                {
                    // All terminating values in read buffer.
                    terminate = true;
                    lastIndex = i + 4;
                }
            }

            if (terminate == false)
            {
                // Offload portion of buffer to emailData, or skip and read again.
                if (strlen(buf) >= 1500)
                {
                    // Offload 1000 characters.
                    dataToAdd.append(buf, 1000);
                    emailData.push_back(dataToAdd);

                    // Remove characters from buffer that were added to emailData.
                    char dataTempArray[2000];
                    strcpy(dataTempArray, buf);
                    memset(buf, '\0', sizeof(buf));
                    int dataBufPosition = 0;

                    for (int j = 1000; j < strlen(dataTempArray); j++)
                    {
                        buf[dataBufPosition++] = dataTempArray[j];
                    }
                }
            }
            else
            {
                // DATA content complete. Gather last string and write to mailbox.
                dataToAdd.append(buf, lastIndex - 2);
                emailData.push_back(dataToAdd);
                // Extract indices for relevant mailbox locks.
                // std::vector<int> mailboxIndices;
                // for (int i = 0; i < mailToAddresses.size(); i++) {
                //     for (int j = 0; mailFileNames.size(); j++) {
                //         if (emailToMailbox[mailToAddresses[i]] == mailFileNames[j]) {
                //             mailboxIndices.push_back(j);
                //             break;
                //         }
                //     }
                // }

                // Calculate time stamp.
                std::time_t timeStamp = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
                std::string timeString = std::ctime(&timeStamp);
                for (int i = 0; i < mailToAddresses.size(); i++)
                {
                    // Add date
                    std::string email_str = "DATE: " + timeString;
                    // Add from without <>
                    email_str += "FROM: " + mailFromAddress.substr(1, mailFromAddress.length() - 2) + "\n";
                    // Add to
                    email_str += "TO: " + mailToAddresses[i] + "\n";
                    std::string message;
                    for (int j = 0; j < emailData.size(); j++)
                    {
                        message += emailData[j];
                    }

                    // Search for subject in message
                    int subject_start = message.find("Subject: ");
                    int newline_pos = message.find("\n", subject_start);
                    email_str += "SUBJECT: " + message.substr(subject_start + 9, newline_pos - subject_start - 9 - 1) + "\n";
                    email_str += "MESSAGE: " + message + "\n";
                    std::string email_str_escaped;
                    for (char ch : email_str)
                    {
                        switch (ch)
                        {
                        case '<':
                            email_str_escaped += "&lt;";
                            break;
                        case '>':
                            email_str_escaped += "&gt;";
                            break;
                        case '&':
                            email_str_escaped += "&amp;";
                            break;
                        case '"':
                            email_str_escaped += "&quot;";
                            break;
                        case '\'':
                            email_str_escaped += "&apos;";
                            break;
                        default:
                            email_str_escaped += ch;
                            break;
                        }
                    }
                    std::cerr << "email_str: " << email_str << std::endl;

                    // Create a message_id that is a hash of the email string
                    unsigned char digest_buf[MD5_DIGEST_LENGTH];
                    computeDigest((char *)email_str.c_str(), strlen(email_str.c_str()), digest_buf);
                    std::string uidl;
                    for (int i = 0; i < MD5_DIGEST_LENGTH; i++)
                    {
                        uidl += "0123456789ABCDEF"[digest_buf[i] / 16];
                        uidl += "0123456789ABCDEF"[digest_buf[i] % 16];
                    }

                    if (mailToAddresses[i].find("@penncloud") != std::string::npos)
                    {
                        std::string recipient_remove_domain = mailToAddresses[i].substr(0, mailToAddresses[i].find("@"));

                        // Ping backend master for backend server address
                        std::string backend_address_port = get_backend_address("email_" + recipient_remove_domain);
                        std::cerr << "Backend address port: " << backend_address_port << std::endl;
                        std::string backend_address = backend_address_port.substr(0, backend_address_port.find(":"));
                        int backend_port = std::stoi(backend_address_port.substr(backend_address_port.find(":") + 1));

                        // Check if user exists
                        std::string user_exists = get_kvs(backend_address, backend_port, "email_" + recipient_remove_domain, "metadata.txt");
                        if (user_exists.length() <= 5 || user_exists.length() > 5 && user_exists.substr(0, 5) != "--ERR")
                        {

                            std::string put_email_response = put_kvs(backend_address, backend_port, "email_" + recipient_remove_domain, uidl + ".txt", email_str_escaped, false, "");
                            std::cerr << "Put email response: " << put_email_response << std::endl;
                            // Run GET and CPUT until successful
                            std::string put_metadata_response;
                            while (put_metadata_response != "+OK Value added")
                            {
                                backend_address_port = get_backend_address("email_" + recipient_remove_domain);
                                backend_address = backend_address_port.substr(0, backend_address_port.find(":"));
                                backend_port = std::stoi(backend_address_port.substr(backend_address_port.find(":") + 1));
                                // std::cerr << "Recipient: " << recipient_remove_domain << std::endl;
                                std::string metadata = get_kvs(backend_address, backend_port, "email_" + recipient_remove_domain, "metadata.txt");
                                // std::cerr << "Get metadata response: " << metadata << std::endl;
                                if (metadata.length() <= 5 || metadata.length() > 5 && metadata.substr(0, 5) != "--ERR")
                                {
                                    put_metadata_response = put_kvs(backend_address, backend_port, "email_" + recipient_remove_domain, "metadata.txt", metadata + uidl + "\n", true, metadata);
                                    // std::cerr << "Put metadata response: " << put_metadata_response << std::endl;
                                }
                            }
                        }
                        else
                        {
                            if (write(comm_FD, &badMailbox[0], badMailbox.length()) < 0)
                            {
                                fprintf(stderr, "DATA complete failed to write: %s\n", strerror(errno));
                            }
                        }
                    }
                }

                // Acquire locks and write to each mailbox.
                // for (int i = 0; i < mailboxIndices.size(); i++) {
                //     std::string filePath;
                //     int flockReturn;

                //     // Correct formatting issue if necessary.
                //     if (mailDirectory[mailDirectory.length() - 1] != '/') {
                //         filePath = mailDirectory + "/" + mailFileNames[mailboxIndices[i]];
                //     } else {
                //         filePath = mailDirectory + mailFileNames[mailboxIndices[i]];
                //     }

                //     // Open file and set descriptor to nonblocking.
                //     int currentFD = open(filePath.c_str(), O_RDWR | O_APPEND);
                //     int fdFlags = fcntl(currentFD, F_GETFL, 0);
                //     fcntl(currentFD, F_SETFL, fdFlags | O_NONBLOCK);
                //     std::string emailHeading = "From " + mailFromAddress + " " + timeString;

                //     // Acquire locks.
                //     while (true) {
                //         flockReturn = flock(currentFD, LOCK_NB | LOCK_EX);

                //         // Lock is being used, wait and try again.
                //         if (flockReturn != 0) {
                //             sleep(3);
                //         } else {
                //             // Lock acquired, continue.
                //             break;
                //         }
                //     }
                //     pthread_mutex_lock(&mailboxLocks[mailboxIndices[i]]);

                //     if (write(currentFD, &emailHeading[0], emailHeading.length()) < 0) {
                //         fprintf(stderr, "Failed to write email heading: %s\n", strerror(errno));
                //     }

                //     for (int j = 0; j < emailData.size(); j++) {
                //         if (write(currentFD, &emailData[j][0], emailData[j].length()) < 0) {
                //             fprintf(stderr, "Failed to write email data: %s\n", strerror(errno));
                //         }
                //     }

                //     flock(currentFD, LOCK_UN);
                //     pthread_mutex_unlock(&mailboxLocks[mailboxIndices[i]]);

                //     close(currentFD);
                // }

                if (write(comm_FD, &okayResponse[0], okayResponse.length()) < 0)
                {
                    fprintf(stderr, "Failed to write email heading: %s\n", strerror(errno));
                }

                // Return to initial state.
                currentState = "Initial";
                emailData.clear();
                mailFromAddress.clear();
                mailToAddresses.clear();

                // Clean buffer.
                char dataTempArray[2000];
                strcpy(dataTempArray, buf);
                memset(buf, '\0', sizeof(buf));
                int dataBufPosition = 0;

                if (lastIndex < 1999)
                {
                    for (int j = lastIndex + 1; j < strlen(dataTempArray); j++)
                    {
                        buf[dataBufPosition++] = dataTempArray[j];
                    }
                }

                // Exit DATA processing.
                dataCalled = false;
            }
        }

        // When shutdown pipe read available, enter shutdown mode.
        int returnValue = select(comm_FD + 1, &fdSet, NULL, NULL, NULL);

        // Server shutdown enabled.
        if (serverShutDown == true)
        {
            break;
        }

        numRead = read(comm_FD, tempBuf, 99);

        // Client has disconnected without requesting QUIT or read() failed. Terminate thread.
        if (numRead <= 0)
        {
            clientDisconnected = true;
            break;
        }

        tempBuf[numRead] = '\0';

        // Copy contents of temp array into full array.
        strncpy(buf + strlen(buf), tempBuf, numRead);

        // Check for complete commands in buffer. Exit loop when no commands remain.
        while (continueReading == false && quitCalled == false && dataCalled == false)
        {
            for (int i = 0; i < strlen(buf) - 1; i++)
            {
                // Last possible command in buffer. Continue reading after loop breaks.
                if (i == strlen(buf) - 2)
                {
                    continueReading = true;
                }

                if (buf[i] == '\r')
                {
                    if (i < strlen(buf) - 1)
                    {
                        if (buf[i + 1] == '\n')
                        {
                            // String is complete. Check for command.

                            if (strlen(buf) < 6)
                            {
                                // Invalid command received.
                                if (write(comm_FD, &invalidCommand[0], invalidCommand.length()) < 0)
                                {
                                    fprintf(stderr, "Failed to write: %s\n", strerror(errno));
                                }

                                // Debugger output - invalid command.
                                if (vFlag == true)
                                {
                                    std::string currentCommand = "";
                                    currentCommand.append(buf, i + 2);
                                    fprintf(stderr, "[%d] S: %s", comm_FD, currentCommand.c_str());
                                    fprintf(stderr, "[%d] R: %s", comm_FD, invalidCommand.c_str());
                                }
                            }
                            else if ((buf[0] == 'H' || buf[0] == 'h') &&
                                     (buf[1] == 'E' || buf[1] == 'e') &&
                                     (buf[2] == 'L' || buf[2] == 'l') &&
                                     (buf[3] == 'O' || buf[3] == 'o') &&
                                     (buf[4] == ' '))
                            {
                                // HELO called, start connection.

                                // 501 and 503 error check.
                                if (strlen(buf) == 7)
                                {
                                    // HELO arg null, reject and return 501 error message.
                                    if (write(comm_FD, &badArgumentError[0], badArgumentError.length()) < 0)
                                    {
                                        fprintf(stderr, "HELO failed to write: %s\n", strerror(errno));
                                    }

                                    // Debugger output - null argument.
                                    if (vFlag == true)
                                    {
                                        std::string currentCommand = "";
                                        currentCommand.append(buf, i + 2);
                                        fprintf(stderr, "[%d] S: %s", comm_FD, currentCommand.c_str());
                                        fprintf(stderr, "[%d] R: %s", comm_FD, badArgumentError.c_str());
                                    }
                                }
                                else if (currentState != "NoHelo" && currentState != "Initial")
                                {
                                    // Invalid state for command.
                                    if (write(comm_FD, &badSequenceOfCommands[0], badSequenceOfCommands.length()) < 0)
                                    {
                                        fprintf(stderr, "HELO failed to write: %s\n", strerror(errno));
                                    }

                                    // Debugger output - bad sequence of commands.
                                    if (vFlag == true)
                                    {
                                        std::string currentCommand = "";
                                        currentCommand.append(buf, i + 2);
                                        fprintf(stderr, "[%d] S: %s", comm_FD, currentCommand.c_str());
                                        fprintf(stderr, "[%d] R: %s", comm_FD, badSequenceOfCommands.c_str());
                                    }
                                }
                                else
                                {
                                    // HELO command accepted. Clear buffers and states.
                                    mailFromAddress.clear();
                                    mailToAddresses.clear();
                                    currentState = "Initial";

                                    if (write(comm_FD, &heloResponse[0], heloResponse.length()) < 0)
                                    {
                                        fprintf(stderr, "HELO failed to write: %s\n", strerror(errno));
                                    }

                                    // Debugger output - HELO command.
                                    if (vFlag == true)
                                    {
                                        std::string currentCommand = "";
                                        currentCommand.append(buf, i + 2);
                                        fprintf(stderr, "[%d] S: %s", comm_FD, currentCommand.c_str());
                                        fprintf(stderr, "[%d] R: %s", comm_FD, heloResponse.c_str());
                                    }
                                }
                            }
                            else if ((buf[0] == 'q' || buf[0] == 'Q') &&
                                     (buf[1] == 'u' || buf[1] == 'U') &&
                                     (buf[2] == 'i' || buf[2] == 'I') &&
                                     (buf[3] == 't' || buf[3] == 'T') &&
                                     (buf[4] == '\r') &&
                                     (buf[5] == '\n'))
                            {
                                // QUIT called, break from loops.
                                quitCalled = true;

                                // Debugger output - QUIT command.
                                if (vFlag == true)
                                {
                                    std::string currentCommand = "";
                                    currentCommand.append(buf, i + 2);
                                    fprintf(stderr, "[%d] S: %s", comm_FD, currentCommand.c_str());
                                }

                                break;
                            }
                            else if ((buf[0] == 'D' || buf[0] == 'd') &&
                                     (buf[1] == 'A' || buf[1] == 'a') &&
                                     (buf[2] == 'T' || buf[2] == 't') &&
                                     (buf[3] == 'A' || buf[3] == 'a') &&
                                     (buf[4] == '\r') &&
                                     (buf[5] == '\n'))
                            {
                                // DATA called, break loop and enter email parsing mode.

                                if (currentState == "RcptTo")
                                {
                                    dataCalled = true;

                                    if (write(comm_FD, &dataInputResponse[0], dataInputResponse.length()) < 0)
                                    {
                                        fprintf(stderr, "DATA acceptance failed to write: %s\n", strerror(errno));
                                    }

                                    // Debugger output - DATA command.
                                    if (vFlag == true)
                                    {
                                        std::string currentCommand = "";
                                        currentCommand.append(buf, i + 2);
                                        fprintf(stderr, "[%d] S: %s", comm_FD, currentCommand.c_str());
                                        fprintf(stderr, "[%d] R: %s", comm_FD, dataInputResponse.c_str());
                                    }
                                }
                                else
                                {
                                    // Invalid sequence of commands.
                                    if (write(comm_FD, &badSequenceOfCommands[0], badSequenceOfCommands.length()) < 0)
                                    {
                                        fprintf(stderr, "DATA acceptance failed to write: %s\n", strerror(errno));
                                    }

                                    // Debugger output - DATA command.
                                    if (vFlag == true)
                                    {
                                        std::string currentCommand = "";
                                        currentCommand.append(buf, i + 2);
                                        fprintf(stderr, "[%d] S: %s", comm_FD, currentCommand.c_str());
                                        fprintf(stderr, "[%d] R: %s", comm_FD, badSequenceOfCommands.c_str());
                                    }
                                }
                            }
                            else if ((buf[0] == 'R' || buf[0] == 'r') &&
                                     (buf[1] == 'S' || buf[1] == 's') &&
                                     (buf[2] == 'E' || buf[2] == 'e') &&
                                     (buf[3] == 'T' || buf[3] == 't') &&
                                     buf[4] == '\r' &&
                                     buf[5] == '\n')
                            {
                                // RSET called, abort mail transaction and clear buffers and states.
                                if (currentState != "NoHelo")
                                {
                                    mailFromAddress.clear();
                                    mailToAddresses.clear();
                                    currentState = "Initial";

                                    if (write(comm_FD, &okayResponse[0], okayResponse.length()) < 0)
                                    {
                                        fprintf(stderr, "RSET failed to write: %s\n", strerror(errno));
                                    }

                                    // Debugger output - RSET command.
                                    if (vFlag == true)
                                    {
                                        std::string currentCommand = "";
                                        currentCommand.append(buf, i + 2);
                                        fprintf(stderr, "[%d] S: %s", comm_FD, currentCommand.c_str());
                                        fprintf(stderr, "[%d] R: %s", comm_FD, okayResponse.c_str());
                                    }
                                }
                                else
                                {
                                    // Wrong sequence of commands. Return error message.
                                    if (write(comm_FD, &badSequenceOfCommands[0], badSequenceOfCommands.length()) < 0)
                                    {
                                        fprintf(stderr, "RSET failed to write: %s\n", strerror(errno));
                                    }

                                    // Debugger output - RSET command.
                                    if (vFlag == true)
                                    {
                                        std::string currentCommand = "";
                                        currentCommand.append(buf, i + 2);
                                        fprintf(stderr, "[%d] S: %s", comm_FD, currentCommand.c_str());
                                        fprintf(stderr, "[%d] R: %s", comm_FD, badSequenceOfCommands.c_str());
                                    }
                                }
                            }
                            else if ((buf[0] == 'N' || buf[0] == 'n') &&
                                     (buf[1] == 'O' || buf[1] == 'o') &&
                                     (buf[2] == 'O' || buf[2] == 'o') &&
                                     (buf[3] == 'P' || buf[3] == 'p') &&
                                     buf[4] == '\r' &&
                                     buf[5] == '\n')
                            {
                                // NOOP called, send OK reply and do nothing if HELO previously sent.
                                if (currentState != "NoHelo")
                                {
                                    if (write(comm_FD, &okayResponse[0], okayResponse.length()) < 0)
                                    {
                                        fprintf(stderr, "NOOP failed to write: %s\n", strerror(errno));
                                    }

                                    // Debugger output - NOOP command.
                                    if (vFlag == true)
                                    {
                                        std::string currentCommand = "";
                                        currentCommand.append(buf, i + 2);
                                        fprintf(stderr, "[%d] S: %s", comm_FD, currentCommand.c_str());
                                        fprintf(stderr, "[%d] R: %s", comm_FD, okayResponse.c_str());
                                    }
                                }
                                else
                                {
                                    // Wrong sequence of commands, send error message.
                                    if (write(comm_FD, &badSequenceOfCommands[0], badSequenceOfCommands.length()) < 0)
                                    {
                                        fprintf(stderr, "NOOP failed to write: %s\n", strerror(errno));
                                    }

                                    // Debugger output - NOOP command.
                                    if (vFlag == true)
                                    {
                                        std::string currentCommand = "";
                                        currentCommand.append(buf, i + 2);
                                        fprintf(stderr, "[%d] S: %s", comm_FD, currentCommand.c_str());
                                        fprintf(stderr, "[%d] R: %s", comm_FD, badSequenceOfCommands.c_str());
                                    }
                                }
                            }
                            else if (strlen(buf) >= 10)
                            {
                                if (strlen(buf) >= 12)
                                {
                                    if ((buf[0] == 'M' || buf[0] == 'm') &&
                                        (buf[1] == 'A' || buf[1] == 'a') &&
                                        (buf[2] == 'I' || buf[2] == 'i') &&
                                        (buf[3] == 'L' || buf[3] == 'l') &&
                                        buf[4] == ' ' &&
                                        (buf[5] == 'F' || buf[5] == 'f') &&
                                        (buf[6] == 'R' || buf[6] == 'r') &&
                                        (buf[7] == 'O' || buf[7] == 'o') &&
                                        (buf[8] == 'M' || buf[8] == 'm') &&
                                        buf[9] == ':')
                                    {
                                        // MAIL FROM: command received.
                                        invalidCmd = false;

                                        // Empty argument case.
                                        if (strlen(buf) == 12)
                                        {
                                            invalidArg = true;
                                        }

                                        // Check for invalid sequence and arguments.
                                        if (currentState != "Initial")
                                        {
                                            // Invalid state for command.
                                            if (write(comm_FD, &badSequenceOfCommands[0], badSequenceOfCommands.length()) < 0)
                                            {
                                                fprintf(stderr, "MAIL FROM failed to write: %s\n", strerror(errno));
                                            }

                                            // Debugger output - bad sequence of commands.
                                            if (vFlag == true)
                                            {
                                                std::string currentCommand = "";
                                                currentCommand.append(buf, i + 2);
                                                fprintf(stderr, "[%d] S: %s", comm_FD, currentCommand.c_str());
                                                fprintf(stderr, "[%d] R: %s", comm_FD, badSequenceOfCommands.c_str());
                                            }
                                        }
                                        else if (invalidArg == false)
                                        {
                                            // Parse the arg and check for errors.
                                            std::string currentCommand = "";
                                            currentCommand.append(buf + 10, i - 8);

                                            if (currentCommand.length() < 7)
                                            {
                                                // Argument too small to be valid.
                                                invalidArg = true;
                                            }
                                            else if (currentCommand[0] != '<' || currentCommand[currentCommand.length() - 3] != '>')
                                            {
                                                // Incorrect brackets.
                                                invalidArg = true;
                                            }
                                            else
                                            {
                                                for (int checkAt = 0; checkAt < currentCommand.length(); checkAt++)
                                                {
                                                    // Check for foo@bar format of input.
                                                    if (currentCommand[checkAt] == '@')
                                                    {
                                                        if (currentCommand[checkAt - 1] == '<' || currentCommand[checkAt + 1] == '>')
                                                        {
                                                            invalidArg = true;
                                                            break;
                                                        }
                                                        else
                                                        {
                                                            break;
                                                        }
                                                    }
                                                }

                                                // Argument is valid. Update 'FROM' address, state, and notify sender.
                                                if (invalidArg == false)
                                                {
                                                    currentState = "MailFrom";
                                                    // TODO: check if mailFromAddress is the full email address
                                                    mailFromAddress = currentCommand.substr(0, currentCommand.length() - 2);

                                                    if (write(comm_FD, &okayResponse[0], okayResponse.length()) < 0)
                                                    {
                                                        fprintf(stderr, "MAIL FROM failed to write: %s\n", strerror(errno));
                                                    }

                                                    // Debugger output.
                                                    if (vFlag == true)
                                                    {
                                                        currentCommand = "";
                                                        currentCommand.append(buf, i + 2);
                                                        fprintf(stderr, "[%d] S: %s", comm_FD, currentCommand.c_str());
                                                        fprintf(stderr, "[%d] R: %s", comm_FD, okayResponse.c_str());
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }

                                if ((buf[0] == 'R' || buf[0] == 'r') &&
                                    (buf[1] == 'C' || buf[1] == 'c') &&
                                    (buf[2] == 'P' || buf[2] == 'p') &&
                                    (buf[3] == 'T' || buf[3] == 't') &&
                                    buf[4] == ' ' &&
                                    (buf[5] == 'T' || buf[5] == 't') &&
                                    (buf[6] == 'O' || buf[6] == 'o') &&
                                    buf[7] == ':')
                                {
                                    // RCPT TO: command received.
                                    invalidCmd = false;

                                    // Empty argument case.
                                    if (strlen(buf) == 10)
                                    {
                                        invalidArg = true;
                                    }

                                    // Check for invalid sequence and arguments.
                                    if (currentState != "MailFrom" && currentState != "RcptTo")
                                    {
                                        // Invalid state for command.
                                        if (write(comm_FD, &badSequenceOfCommands[0], badSequenceOfCommands.length()) < 0)
                                        {
                                            fprintf(stderr, "RCPT TO failed to write: %s\n", strerror(errno));
                                        }

                                        // Debugger output - bad sequence of commands.
                                        if (vFlag == true)
                                        {
                                            std::string currentCommand = "";
                                            currentCommand.append(buf, i + 2);
                                            fprintf(stderr, "[%d] S: %s", comm_FD, currentCommand.c_str());
                                            fprintf(stderr, "[%d] R: %s", comm_FD, badSequenceOfCommands.c_str());
                                        }
                                    }
                                    else if (invalidArg == false)
                                    {
                                        // Parse the arg and check for errors.
                                        std::string currentCommand = "";
                                        currentCommand.append(buf + 8, i - 6);

                                        if (currentCommand.length() < 7)
                                        {
                                            // Argument too small to be valid.
                                            invalidArg = true;
                                        }
                                        else if (currentCommand[0] != '<' || currentCommand[currentCommand.length() - 3] != '>')
                                        {
                                            // Incorrect brackets.
                                            invalidArg = true;
                                        }
                                        else
                                        {
                                            for (int checkAt = 0; checkAt < currentCommand.length(); checkAt++)
                                            {
                                                // Check for foo@bar format of input.
                                                if (currentCommand[checkAt] == '@')
                                                {
                                                    if (currentCommand[checkAt - 1] == '<' || currentCommand[checkAt + 1] == '>')
                                                    {
                                                        invalidArg = true;
                                                        break;
                                                    }
                                                    else
                                                    {
                                                        break;
                                                    }
                                                }
                                            }

                                            // Check if mailbox exists in our directory.
                                            bool mailBoxExists = false;
                                            std::string mailbox = "";
                                            // TODO: check if mailbox is the full email address
                                            if (invalidArg == false)
                                            {
                                                mailbox.append(buf + 9, i - 10);
                                                mailBoxExists = true;
                                                // for (int vecSearch = 0; vecSearch < emailNames.size(); vecSearch++) {
                                                //     if (emailNames[vecSearch] == mailbox) {
                                                //         mailBoxExists = true;
                                                //         break;
                                                //     }
                                                // }

                                                // Invalid mailbox error.
                                                if (mailBoxExists == false)
                                                {
                                                    if (write(comm_FD, &badMailbox[0], badMailbox.length()) < 0)
                                                    {
                                                        fprintf(stderr, "RCPT TO failed to write: %s\n", strerror(errno));
                                                    }

                                                    // Debugger output.
                                                    if (vFlag == true)
                                                    {
                                                        currentCommand = "";
                                                        currentCommand.append(buf, i + 2);
                                                        fprintf(stderr, "[%d] S: %s", comm_FD, currentCommand.c_str());
                                                        fprintf(stderr, "[%d] R: %s", comm_FD, badMailbox.c_str());
                                                    }
                                                }
                                            }

                                            // Argument is valid. Update RCPT address, state, and notify sender.
                                            if (invalidArg == false && mailBoxExists == true)
                                            {
                                                currentState = "RcptTo";
                                                mailToAddresses.push_back(mailbox);

                                                if (write(comm_FD, &okayResponse[0], okayResponse.length()) < 0)
                                                {
                                                    fprintf(stderr, "RCPT TO failed to write: %s\n", strerror(errno));
                                                }

                                                // Debugger output.
                                                if (vFlag == true)
                                                {
                                                    currentCommand = "";
                                                    currentCommand.append(buf, i + 2);
                                                    fprintf(stderr, "[%d] S: %s", comm_FD, currentCommand.c_str());
                                                    fprintf(stderr, "[%d] R: %s", comm_FD, okayResponse.c_str());
                                                }
                                            }
                                        }
                                    }
                                }

                                // Handle invalid command and argument errors.
                                if (invalidCmd == true)
                                {
                                    if (write(comm_FD, &invalidCommand[0], invalidCommand.length()) < 0)
                                    {
                                        fprintf(stderr, "Failed to write: %s\n", strerror(errno));
                                    }

                                    // Debugger output.
                                    if (vFlag == true)
                                    {
                                        std::string currentCommand = "";
                                        currentCommand.append(buf, i + 2);
                                        fprintf(stderr, "[%d] S: %s", comm_FD, currentCommand.c_str());
                                        fprintf(stderr, "[%d] R: %s", comm_FD, invalidCommand.c_str());
                                    }
                                }
                                else if (invalidArg == true)
                                {
                                    invalidArg = false;

                                    if (write(comm_FD, &badArgumentError[0], badArgumentError.length()) < 0)
                                    {
                                        fprintf(stderr, "Failed to write: %s\n", strerror(errno));
                                    }

                                    // Debugger output.
                                    if (vFlag == true)
                                    {
                                        std::string currentCommand = "";
                                        currentCommand.append(buf, i + 2);
                                        fprintf(stderr, "[%d] S: %s", comm_FD, currentCommand.c_str());
                                        fprintf(stderr, "[%d] R: %s", comm_FD, badArgumentError.c_str());
                                    }
                                }
                                else
                                {
                                    // Previous command was valid. Reset command checker.
                                    invalidCmd = true;
                                }
                            }
                            else
                            {
                                // Invalid command received.
                                if (write(comm_FD, &invalidCommand[0], invalidCommand.length()) < 0)
                                {
                                    fprintf(stderr, "Failed to write: %s\n", strerror(errno));
                                }

                                // Debugger output - invalid command.
                                if (vFlag == true)
                                {
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

                            for (int j = i + 2; j < strlen(tempArray); j++)
                            {
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

    if (serverShutDown == false)
    {
        if (clientDisconnected == false)
        {
            // Quit requested. Send farewell message and close this connection.
            write(comm_FD, &quitMessage[0], quitMessage.length());

            // Debugger output - farewell message.
            if (vFlag == true)
            {
                fprintf(stderr, "[%d] R: %s", comm_FD, quitMessage.c_str());
            }
        }

        // Close the connection and update active fileDescriptors.
        close(comm_FD);

        // Debugger output - connection closed.
        if (vFlag == true)
        {
            fprintf(stderr, "[%d] Connection closed\n", comm_FD);
        }
    }
    else
    {
        // Server shutting down. Write message to client and close connection.
        write(comm_FD, &quitMessage[0], quitMessage.length());

        // Debugger output - server shutdown enabled and connection closed.
        if (vFlag == true)
        {
            fprintf(stderr, "[%d] R: %s", comm_FD, quitMessage.c_str());
            fprintf(stderr, "[%d] Connection closed\n", comm_FD);
        }

        close(comm_FD);
    }

    // Update active thread information, clean up memory, and exit.
    pthread_mutex_lock(&threadUpdatesLock);
    closedConnections.push_back(static_cast<threadDetails *>(connectionInfo)->myIndex);
    pthread_mutex_unlock(&threadUpdatesLock);

    delete static_cast<threadDetails *>(connectionInfo);

    pthread_exit(NULL);
}

void signalHandler(int signal)
{
    serverShutDown = true;
    char *shutdownSignal = new char;
    *shutdownSignal = 'X';

    // Separate SIGINT from new output.
    fprintf(stderr, "\n");

    // Write the shutdown signal for threads to see.
    write(shutDownPipe[1], shutdownSignal, 1);

    delete shutdownSignal;
}