CXX = g++ -g -fPIC 
NETLIBS= -lnsl

all: daytime-server use-dlopen hello.so myhttpd

daytime-server : daytime-server.o
	$(CXX) -o $@ $@.o $(NETLIBS)

use-dlopen: use-dlopen.o
	$(CXX) -o $@ $@.o $(NETLIBS) -ldl

hello.so: hello.o
	ld -G -o hello.so hello.o

myhttpd: myhttpd.cpp
	$(CXX) -o myhttpd myhttpd.cpp -lpthread -ldl

%.o: %.cc
	@echo 'Building $@ from $<'
	$(CXX) -o $@ -c -I. $<

clean:
	rm -f *.o use-dlopen hello.so
	rm -f *.o daytime-server
	rm -f *.o myhttpd

