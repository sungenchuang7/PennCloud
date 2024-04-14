TARGETS = backendserver

all: $(TARGETS)

backendserver: backendserver.cc
	g++ $^ -lpthread -g -o $@

clean::
	rm -fv $(TARGETS) *~