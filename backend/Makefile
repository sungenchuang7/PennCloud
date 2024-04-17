TARGETS = backendserver masterbackendserver

all: $(TARGETS)

backendserver: backendserver.cc
	g++ $^ -std=c++14 -lpthread -g -o $@

masterbackendserver: masterbackendserver.cc
	g++ $^ -std=c++14 -lpthread -g -o $@

clean::
	rm -fv $(TARGETS) *~
