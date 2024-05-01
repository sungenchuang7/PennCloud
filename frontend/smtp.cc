#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <unistd.h>
#include <fstream>
#include <vector>
#include <arpa/inet.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <iostream>
#include <chrono>
#include <sys/file.h>
#include <dirent.h>
#include <map>


//global variables for threads/comm_fds
static std::vector<pthread_t> threads;
static std::vector<int> comm_fds;
static bool vflag = false;
static bool shutting_down = false;
static std::string mailbox;
static std::map<std::string, int> mutex_indices;
static std::vector<pthread_mutex_t> mutexes;

//mask to block SIGINT 
void mask_sig() {
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGINT);

  pthread_sigmask(SIG_BLOCK, &mask, NULL);
}

//handler for sigint
void handle_sigint(int sig) {
  shutting_down = true;
  char goodbye_msg[] = "-ERR Server shutting down\r\n";
  //iterate through all threads/open fds and send msg and then close
  for (int i = 0; i < threads.size(); i++) {
    int curr_comm_fd = comm_fds[i];
    int ret = fcntl(curr_comm_fd, F_SETFL, fcntl(curr_comm_fd, F_GETFL, 0) | O_NONBLOCK);
    if (ret < 0) {
      fprintf(stderr, "fcntl error\n");
      exit(1);
    }
    write(curr_comm_fd, goodbye_msg, strlen(goodbye_msg));
    if (vflag) {
      fprintf(stderr, "[%d] S: %s", curr_comm_fd, goodbye_msg);
    }
    close(curr_comm_fd);
    if (vflag) {
      fprintf(stderr, "[%d] Connection closed\n", curr_comm_fd);
    }
    pthread_cancel(threads[i]);
  }
  exit(1);
}

void *threadFunc(void *arg) {
  //start by blocking all SIGINT signals in the threads
  mask_sig();
  int comm_fd = *(int *) arg;
  // we'll assume the length of a command is always at most 1000, so allocating 2000 characters to the buffer will always be sufficient
  char* buffer = (char*) malloc(2000);
  bzero(buffer, 2000);
  int total_chars = 0;
  std::string state = "none"; //initialize state string to "none" to track what state the connection is in
  std::string sender = ""; // initialize sender string 
  std::vector<std::string> rcpts; //initialize vector of rcpts 
  std::string msg = ""; //initialize msg string

  //state constants
  std::string initial("initial");
  std::string none("none");
  std::string from("from");
  std::string to("to");
  std::string data("data");

  //error messages 
  char syntax_msg[] = "501 Syntax Error\r\n";
  char good_hello[] = "250 localhost\r\n";
  char bad_commands[] = "503 Bad sequence of commands\r\n";
  char success[] = "250 OK\r\n";
  char ready_data[] = "354 ready for data\r\n";
  char fake_rcpt[] = "550 recipient does not exist\r\n";
  char blocked[] = "451 files are locked\r\n";


  while(true) {
    //read 100 bytes into a read buffer
    char* read_buffer = (char*) malloc(100);
    for (int i = 0; i < 100; i++) {
      read_buffer[i] = '\0';
    }
    int num_read = read(comm_fd, read_buffer, 100);
    if (num_read < 0) {
      fprintf(stderr, "read failed!\n");
      exit(1);
    }
    if (num_read == 0) {
      // this means the client has closed the connection
      free(read_buffer);
      close(comm_fd);
      // remove comm_fd and thread from global vector
      for (int i = 0; i < threads.size(); i++) {
        if (comm_fds[i] == comm_fd) {
          comm_fds.erase(comm_fds.begin() + i);
          threads.erase(threads.begin() + i);
        }
      }
      if (vflag) {
        fprintf(stderr, "[%d] Connection closed\n", comm_fd);
      }
      free(buffer);
      pthread_exit(NULL);
    }
    if (vflag) {
      fprintf(stderr, "[%d] C: %s", comm_fd, read_buffer);
    }

    //copy that read buffer into the overall command buffer
    for (int i = 0; i < 100; i++) {
      buffer[total_chars + i] = read_buffer[i];
    }
    total_chars = total_chars + num_read;
    free(read_buffer);

    //iterate through buffer to check for command
    int i = 0;
    while (buffer[i] != '\0') {
      if (buffer[i] == '\r' && buffer[i + 1] == '\n') {
        //parse command here
        //if we've already received DATA command, don't parse for commands 
        if (state.compare(data) == 0) {
          //check if we should send email: 
          if (buffer[0] == '.' && i == 1) {
            const char* msg_array = msg.c_str();
            bool successful_send = false;
            // iterate throught recipients and try to send email
            for (auto rcpt : rcpts) {
              // get time 
              const auto now = std::chrono::system_clock::now();
              const std::time_t t_c = std::chrono::system_clock::to_time_t(now);
              std::string direct = mailbox;
              std::string path = direct + "/" + rcpt + ".mbox";
              const char* path_array = path.c_str();
              //open file in append mode
              int fp = open(path_array, O_RDWR | O_APPEND);
              if (fp == -1) {
                perror("open");
              }
              // lock from other processes (nonblocking)
              if (flock(fp, LOCK_EX | LOCK_NB) == 0) {
                //find the correct mutex and lock it (nonblocking)
                int mutex_index = mutex_indices[rcpt];
                pthread_mutex_t curr_mutex = mutexes[mutex_index];
                if (pthread_mutex_trylock(&curr_mutex) == 0) {
                  FILE *file = fdopen(fp, "a");
                  if (file == NULL) {
                    perror("fdopen");
                  }
                  fprintf(file, "From %s %s", sender.c_str(), std::ctime(&t_c));
                  fprintf(file, "%s", msg_array);
                  //unlock flock and mutex
                  flock(fp, LOCK_UN);
                  pthread_mutex_unlock(&curr_mutex);
                  fclose(file);
                  successful_send = true;
                }
              }
              close(fp);
            }
            //if any of the rcpts go through, successful_send is true and we should return success
            // if all of them fail (because the files are locked), return error
            if (successful_send) {
              write(comm_fd, success, strlen(success));
              if (vflag) {
                fprintf(stderr, "[%d] S: %s", comm_fd, success);
              }
            } else {
              // all recipients are blocked, throw error
              write(comm_fd, blocked, strlen(blocked));
              if (vflag) {
                fprintf(stderr, "[%d] S: %s", comm_fd, blocked);
              }
            }
            //reset state
            sender = "";
            rcpts.clear();
            msg = "";
            state = "initial";
          // otherwise, we're still in DATA and we just need to store the data in msg string
          } else {
            char* text = (char*) malloc(i + 3);
            for (int j = 0; j < i+2; j++) {
              text[j] = buffer[j];
            }
            text[i+2] = '\0';
            std::string new_text = text;
            msg = msg + new_text;
            free(text);
          }
        } else if (toupper(buffer[0]) == 'H' && toupper(buffer[1]) == 'E' && toupper(buffer[2]) == 'L' && toupper(buffer[3]) == 'O') {
          // check to make sure something is specified in HELO
          if (buffer[4] == '\0' || buffer[5] == '\0' || i < 5) {
            write(comm_fd, syntax_msg, strlen(syntax_msg));
            if (vflag) {
              fprintf(stderr, "[%d] S: %s", comm_fd, syntax_msg);
            }
          } else if (state.compare(none) == 0 || state.compare(initial) == 0) {
            // check if we're in the right state
            write(comm_fd, good_hello, strlen(good_hello));
            if (vflag) {
              fprintf(stderr, "[%d] S: %s", comm_fd, good_hello);
            }
            state = "initial";
          } else {
            write(comm_fd, bad_commands, strlen(bad_commands));
            if (vflag) {
              fprintf(stderr, "[%d] S: %s", comm_fd, bad_commands);
            }
          }
        } else if (toupper(buffer[0]) == 'M' && toupper(buffer[1]) == 'A' && toupper(buffer[2]) == 'I' && toupper(buffer[3]) == 'L' && buffer[4] == ' ' && toupper(buffer[5]) == 'F' && toupper(buffer[6]) == 'R' && toupper(buffer[7]) == 'O' && toupper(buffer[8]) == 'M' && buffer[9] == ':') {
          // make sure we've said helo; if haven't, throw error
          if (state.compare(initial) != 0) {
            write(comm_fd, bad_commands, strlen(bad_commands));
            if (vflag) {
              fprintf(stderr, "[%d] S: %s", comm_fd, bad_commands);
            }
          } else {
            // parse thorugh the rest of the command
            char* text = (char*) malloc(i - 9);
            for (int j = 10; j < i; j++){
              text[j - 10] = buffer[j];
            }
            text[i-10] = '\0';
            fprintf(stderr, "%s %d\n", text, i);
            // check if the input is bracketed
            if (text[0] != '<' || text[i-11] != '>') {
              write(comm_fd, syntax_msg, strlen(syntax_msg));
              if (vflag) {
                fprintf(stderr, "[%d] S: %s", comm_fd, syntax_msg);
              }
              free(text);
            } else {
              // if it is, update sender variable and return success
              sender = text;
              free(text);
              write(comm_fd, success, strlen(success));
              state = "from";
              if (vflag) {
                fprintf(stderr, "[%d] S: %s", comm_fd, success);
              }
            }
          }
        } else if (toupper(buffer[0]) == 'R' && toupper(buffer[1]) == 'C' && toupper(buffer[2]) == 'P' && toupper(buffer[3]) == 'T' && buffer[4] == ' ' && toupper(buffer[5]) == 'T' && toupper(buffer[6]) == 'O' && buffer[7] == ':') {
          // make sure we've specified a sender 
          if (state.compare(from) != 0 && state.compare(to) != 0) {
            write(comm_fd, bad_commands, strlen(bad_commands));
            if (vflag) {
              fprintf(stderr, "[%d] S: %s", comm_fd, bad_commands);
            }
          } else {
            // read rest of command 
            char* text = (char*) malloc(i - 7);
            for (int j = 8; j < i; j++){
              text[j - 8] = buffer[j];
            }
            text[i-8] = '\0';
            // send error if text doesn't end in @localhost and isn't bracketed
            if (text[0] != '<' || text[i-19] != '@' || text[i-18] != 'l' || text[i-17] != 'o' || text[i-16] != 'c' || text[i-15] != 'a' || text[i-14] != 'l' || text[i-13] != 'h' || text[i-12] != 'o' ||text[i-11] != 's' ||text[i-10] != 't' || text[i-9] != '>') {
              write(comm_fd, syntax_msg, strlen(syntax_msg));
              if (vflag) {
                fprintf(stderr, "[%d] S: %s", comm_fd, syntax_msg);
              }
              free(text);
            } else {
              std::string rcpt = text;
              // trim the < and > characters
              rcpt = rcpt.substr(1, rcpt.size() - 12);
              std::string direct = mailbox;
              std::string path = direct + "/" + rcpt + ".mbox";
              const char* path_array = path.c_str();
              // check if recipient file exists
              if (access(path_array, F_OK) == -1) {
                write(comm_fd, fake_rcpt, strlen(fake_rcpt));
                if (vflag) {
                  fprintf(stderr, "[%d] S: %s", comm_fd, fake_rcpt);
                }
              } else {
                // if it exists, add the rcpt to the rcpts vector and return success
                rcpts.push_back(rcpt);
                write(comm_fd, success, strlen(success));
                state = "to";
                if (vflag) {
                  fprintf(stderr, "[%d] S: %s", comm_fd, success);
                }
              }
              free(text);
            }
          }
        } else if (toupper(buffer[0]) == 'D' && toupper(buffer[1]) == 'A' && toupper(buffer[2]) == 'T' && toupper(buffer[3]) == 'A') {
          // make sure that sender/rcpts are specified 
          if (state.compare(to) != 0) {
            write(comm_fd, bad_commands, strlen(bad_commands));
            if (vflag) {
              fprintf(stderr, "[%d] S: %s", comm_fd, bad_commands);
            }
          }
          write(comm_fd, ready_data, strlen(ready_data));
          //mark state as ready for data
          state = "data";
          if (vflag) {
            fprintf(stderr, "[%d] S: %s", comm_fd, ready_data);
          }
        } else if (toupper(buffer[0]) == 'R' && toupper(buffer[1]) == 'S' && toupper(buffer[2]) == 'E' && toupper(buffer[3]) == 'T') {
          // check if we've said helo 
          if (state.compare(none) == 0) {
            write(comm_fd, bad_commands, strlen(bad_commands));
            if (vflag) {
              fprintf(stderr, "[%d] S: %s", comm_fd, bad_commands);
            }
          } else {
            // reset everything and bump state back to initial
            sender = "";
            rcpts.clear();
            msg = "";
            state = "initial";
          }
        } else if (toupper(buffer[0]) == 'N' && toupper(buffer[1]) == 'O' && toupper(buffer[2]) == 'O' && toupper(buffer[3]) == 'P') {
          // check if we've said helo
          if (state.compare(none) == 0) {
            write(comm_fd, bad_commands, strlen(bad_commands));
            if (vflag) {
              fprintf(stderr, "[%d] S: %s", comm_fd, bad_commands);
            }
          } else {
            // do nothing 
            write(comm_fd, success, strlen(success));
            if (vflag) {
              fprintf(stderr, "[%d] S: %s", comm_fd, success);
            }
          }
        } else if (toupper(buffer[0]) == 'Q' && toupper(buffer[1]) == 'U' && toupper(buffer[2]) == 'I' && toupper(buffer[3]) == 'T') {
          char goodbye_msg[] = "221 Goodbye!\r\n";
          write(comm_fd, goodbye_msg, strlen(goodbye_msg));
          if (vflag) {
            fprintf(stderr, "[%d] S: %s", comm_fd, goodbye_msg);
          }
          close(comm_fd);
          // remove comm_fd and thread from the global vector
          for (int j = 0; j < threads.size(); j++) {
            if (comm_fds[j] == comm_fd) {
              comm_fds.erase(comm_fds.begin() + j);
              threads.erase(threads.begin() + j);
            }
          }
          if (vflag) {
            fprintf(stderr, "[%d] Connection closed\n", comm_fd);
          }
          free(buffer);
          pthread_exit(NULL);
        } else {
          char error_msg[] = "500 Command not recognized\r\n";
          write(comm_fd, error_msg, strlen(error_msg));
          if (vflag) {
            fprintf(stderr, "[%d] S: %s", comm_fd, error_msg);
          }
        }
        //move up everything that wasn't read
        for (int j = 0; j < i + 2; j++) {
          buffer[j] = '\0';
        }
        for (int j = i + 2; j < 2000; j++) {
          buffer[j - i - 2] = buffer[j];
          buffer[j] = '\0';
        }
        total_chars = total_chars - i - 2;
        i = 0;
      } else {
        i++;
      }
    }
  }

  pthread_exit(NULL);
}

int main(int argc, char *argv[])
{
  signal(SIGINT, handle_sigint);
  int p = 2500; //default port = 2500
	int c; // var for getopt

	while ((c = getopt (argc, argv, "avp:")) != -1)
		switch (c)
		{
			case 'v':
				vflag = true;
				break;
			case 'p':
				p = atoi(optarg);
				break;
      case 'a':
        fprintf(stderr, "Author: Andrew Jiang (ajay54)\n");
		    return 1;
			case '?':
				if (optopt == 'p')
					fprintf(stderr, "Option -%c requires an argument. \n", optopt);
				else if (isprint (optopt))
					fprintf(stderr, "Unknown option '-%c'.\n", optopt);
				else
					fprintf(stderr, "Unknown option character '\\x%x',\n", optopt);
				return 1;
			default:
				abort();
		}
  // store input directory
  for (; optind < argc; optind++) {
    mailbox = argv[optind];
  }
  if (mailbox.length() == 0) {
    fprintf(stderr, "Server requires a specified mailbox directory!\n");
    return -1;
  }

  //count how many mbox files in the directory
  int file_count = 0;
  DIR * dir;
  struct dirent * curr;
  dir = opendir(mailbox.c_str());
  if (dir == NULL) {
    fprintf(stderr, "Directory does not exist!\n");
    return -1;
  }
  while((curr = readdir(dir)) != NULL) {
    char *name = curr->d_name;
    std::string str_name = name;
    int mutex_size = mutexes.size();
    size_t len = strlen(name);
    if (len > 5 && strcmp(name + len - 5, ".mbox") == 0) {
      pthread_mutex_t new_mutex; 
      pthread_mutex_init(&new_mutex, NULL);
      mutex_indices.insert(std::pair<std::string, int>(str_name, mutex_size));
      mutexes.push_back(new_mutex);
    }
  }

  //initialize socket

  int sockfd = socket(PF_INET, SOCK_STREAM, 0);
  if (sockfd < 0) {
    fprintf(stderr, "Cannot open socket (%s)\n", strerror(errno));
    exit(1);
  }

  //bind socket to specified port

  struct sockaddr_in servaddr;
  bzero(&servaddr, sizeof(servaddr));

  servaddr.sin_family = AF_INET;
  servaddr.sin_addr.s_addr = htons(INADDR_ANY);
  servaddr.sin_port = htons(p);

  int opt = 1;
  int ret = setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR|SO_REUSEPORT, &opt, sizeof(opt));
  if (ret < 0) {
    fprintf(stderr, "setsockopt failed\n");
    exit(1);
  }

  ret = bind(sockfd, (struct sockaddr*) &servaddr, sizeof(servaddr));
  if (ret < 0) {
    fprintf(stderr, "failed to bind (%s)\n", strerror(errno));
    exit(1);
  }

  //start listening
  listen(sockfd, 100);

  while (true) {
    struct sockaddr_in clientaddr;
    socklen_t clientaddrlen = sizeof(clientaddr);
    //accept connection
    int comm_fd = accept(sockfd, (struct sockaddr*) &clientaddr, &clientaddrlen);
    char ready_msg[] = "220 localhost READY\r\n";
    write(comm_fd, ready_msg, strlen(ready_msg));
    if (vflag) {
      fprintf(stderr, "[%d] New connection\n", comm_fd);
      fprintf(stderr, "S: %s", ready_msg);
    }
    
    //create thread for this connection
    pthread_t temp_threads;
    if (pthread_create(&temp_threads, NULL, threadFunc, &comm_fd) != 0) {
				fprintf(stderr, "pthread_create error");
				return -1;
		}

    //check if shutting down; if so, close the fd and send msg
    if (shutting_down) {
      char goodbye_msg[] = "-ERR Server shutting down\r\n";
      write(comm_fd, goodbye_msg, strlen(goodbye_msg));
      if (vflag) {
        fprintf(stderr, "[%d] S: %s", comm_fd, goodbye_msg);
      }
      close(comm_fd);
      if (vflag) {
        fprintf(stderr, "[%d] Connection closed\n", comm_fd);
      }
      pthread_cancel(temp_threads);
    } else {
      threads.push_back(temp_threads);
      comm_fds.push_back(comm_fd);
    }
  }

  return 0;
}
