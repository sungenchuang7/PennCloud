TARGETS = frontendserver loadbalancer smtpserver

all: $(TARGETS)

frontendserver: frontendserver.cc routes.cc
	g++ -std=c++17 $(shell pkg-config --cflags uuid) -I/usr/local/opt/openssl/include $^ $(shell pkg-config --libs uuid) -L/usr/local/opt/openssl/lib -lcrypto -lpthread -g -o $@
	 
loadbalancer: loadbalancer.cc
	g++ -std=c++17 $^ -lpthread -g -o $@

smtpserver: smtp-server.cc routes.cc
	g++ -std=c++17 $(shell pkg-config --cflags uuid) $^ -I/usr/local/opt/openssl/include $(shell pkg-config --libs uuid) -L/usr/local/opt/openssl/lib -lcrypto -lpthread -g -o $@

clean::
	rm -fv $(TARGETS) *~