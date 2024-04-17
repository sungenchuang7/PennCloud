TARGETS = backendserver masterbackendserver

all: $(TARGETS)

backendserver: backendserver.cc
	g++ $^ -lpthread -g -o $@

masterbackendserver: masterbackendserver.cc
	g++ $^ -lpthread -g -o $@

clean::
	rm -fv $(TARGETS) *~