
const char * usage =
"                                                               \n"
"MyAwesomeServer:                                                \n"
"                                                               \n"
"Server that can run html codes, browse and display directories \n"
"and run CGI-Scripts with arguments                             \n"
"                                                               \n"
"To use it in one window type:                                  \n"
"                                                               \n"
"   myhttpd [-f|-t|-p] <port>                                   \n"
"                                                               \n"
"Where 1024 < port < 65536.                                     \n"
"You can choose one of the flags (or none) between myhttpd		\n"
"and your port number. No flags runs myhttpd in iterative mode. \n"
"The -f flag runs a process based version of myhttpd. The -t    \n"
"flag runs a thread based version of myhttpd. The -p flag runs  \n"
"a thread pool based version of myhttpd.						\n"
"                                                               \n"
"In another window type:                                       \n"
"                                                               \n"
"   telnet <host> <port>                                        \n"
"                                                               \n"
"where <host> is the name of the machine where myhttpd          \n"
"is running. <port> is the port number you used when you run   \n"
"myhttpd.                                                      \n"
"                                                               \n"
"In your browser, type:	(Please only run in Chromium)			\n"
"																\n"
"	<host>:<port>												\n"
"																\n"
"where <host> is the name of the machine where myhttpd          \n"
"is running. <port> is the port number you used when you run   \n"
"myhttpd. Here, you can browse and run all sort of things!     \n"
"																\n"
"To exit type Ctrl-z											\n";

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <dirent.h>
#include <dlfcn.h>
#include <ctime>
#include <sys/mman.h>

int QueueLength = 5;
int port;
time_t startTime;
int numRequests;

double minTime = -1;
char* minChar;
double maxTime = -1;
char* maxChar;

struct FileStats {
	struct dirent *dir;
	char *p;
	char *dp;
	struct stat b;
};

void processRequest(int slaveSocket);
void processRequestThread(int* slaveSocket);
void poolSlave(int* socket);
void processFilePath(int slaveSocket, char *docpath, char *args);
void processCGIBin(int slaveSocket, char *docpath, char* real_path, char *args);
void runLoadable(int slaveSocket, char *real_path, char *args);

int endsWith(char *first, const char *second);
char *getParentDirectory(char *child);
char *numToString(int num);
char *sizeToString(int num);
char *timeToString(double num);
char *getArgs(char *dir);
char *removeArgs(char *dir);
char *getScript(char *docpath);
void compareTimes(double time, char* real_path);

void sortFiles(struct FileStats **files, int numFiles, char *args);
int compareNA(const void* a, const void* b);
int compareMA(const void* a, const void* b);
int compareSA(const void* a, const void* b);
int compareND(const void* a, const void* b);
int compareMD(const void* a, const void* b);
int compareSD(const void* a, const void* b);

void send200(int slaveSocket, char *contentType, int fd);
void send403(int slaveSocket);
void send404(int slaveSocket, char *contentType);
void sendDir(int slaveSocket, DIR * d, char *docpath, char *real_path, char *contentType, char *args);
void sendStats(int slaveSocket, char *real_path);
void addToLog(int sock, char *directory);

pthread_mutex_t mutex;
pthread_mutex_t wmutex;

extern void killzombie(int sig) {
	while(waitpid(-1, NULL, WNOHANG) > 0);
}

extern void ignC(int sig) {
	while(waitpid(-1, NULL, WNOHANG) > 0);
}

extern void ignP(int sig) {
	signal(SIGPIPE,SIG_IGN);
}

//to test in browser, type "localhost:#" where # stands for port number you chose
//note for later: pthread detatch

int main( int argc, char ** argv ) {

	//zombies
	struct sigaction signalAction;
	signalAction.sa_handler = killzombie;
	sigemptyset(&signalAction.sa_mask);
	signalAction.sa_flags = SA_RESTART;
	int e = sigaction(SIGCHLD, &signalAction, NULL );

	//ignoring ctrl-c
	struct sigaction signalActionC;
	signalActionC.sa_handler = ignC;
	sigemptyset(&signalActionC.sa_mask);
	signalActionC.sa_flags = SA_RESTART;
	int errorC = sigaction(SIGTSTP, &signalActionC, NULL );

	//broken pipe
	struct sigaction signalActionP;
	signalActionP.sa_handler = ignP;
	sigemptyset(&signalActionP.sa_mask);
	signalActionP.sa_flags = SA_RESTART;
	int errorP = sigaction(SIGPIPE, &signalActionP, NULL );

	if ( argc < 2 ) {
    	fprintf( stderr, "%s", usage );
    	port = 3332;
  	} else {
		// Get the port from the arguments
		port = atoi( argv[1] );
	}
	time(&startTime);

	pthread_mutex_init(&wmutex, NULL);

	int process_based = 0;
	int thread_based = 0;
	int pool_based = 0;
	for (int i = 1; i < argc; i++) {
		if (!strncmp(argv[i], "-f", 2)) {
			port = atoi( argv[2] );
			process_based = 1;
		} else if (!strncmp(argv[i], "-t", 2)) {
			thread_based = 1;
			port = atoi( argv[2] );
		} else if (!strncmp(argv[i], "-p", 2)) {
			pool_based = 1;
			port = atoi( argv[2] );
		}
	}

	// Set the IP address and port for this server
	struct sockaddr_in serverIPAddress; 
	memset( &serverIPAddress, 0, sizeof(serverIPAddress) );
	serverIPAddress.sin_family = AF_INET;
	serverIPAddress.sin_addr.s_addr = INADDR_ANY;
	serverIPAddress.sin_port = htons((u_short) port);

	// Allocate a socket
	int masterSocket =  socket(PF_INET, SOCK_STREAM, 0);
	if ( masterSocket < 0) {
		perror("socket");
		exit( -1 );
	}

	// Set socket options to reuse port. Otherwise we will
	// have to wait about 2 minutes before reusing the sae port number
	int optval = 1; 
	int err = setsockopt(masterSocket, SOL_SOCKET, SO_REUSEADDR, 
	   (char *) &optval, sizeof( int ) );
	   
	// Bind the socket to the IP address and port
	int error = bind( masterSocket, (struct sockaddr *)&serverIPAddress,
		sizeof(serverIPAddress) );
	if ( error ) {
		perror("bind");
		exit( -1 );
	}

	// Put socket in listening mode and set the 
	// size of the queue of unprocessed connections
	error = listen( masterSocket, QueueLength);
	if ( error ) {
	    perror("listen");
	    exit( -1 );
	}

	if (process_based) {
		while(1) {
			 //Accept new TCP connection
			struct sockaddr_in clientIPAddress;
			int alen = sizeof( clientIPAddress );
			int slaveSocket = accept( masterSocket, 
				(struct sockaddr *)&clientIPAddress, (socklen_t*)&alen);

			if ( slaveSocket < 0 ) {
				perror( "accept" );
				exit( -1 );
			}
			numRequests++;
			pid_t slave = fork();
			if (slave == 0) {
				//Read request from TCP connection and parse it.
				processRequest(slaveSocket);
				//Close TCP connection
				shutdown( slaveSocket, SHUT_RDWR );
				close( slaveSocket );
				exit(0);
			}
			//Close TCP connection
			close( slaveSocket );
		}
	} else if (thread_based) {
		while(1) {
			 //Accept new TCP connection
			struct sockaddr_in clientIPAddress;
			int alen = sizeof( clientIPAddress );
			int slaveSocket = accept( masterSocket, 
				(struct sockaddr *)&clientIPAddress, (socklen_t*)&alen);

			if ( slaveSocket < 0 ) {
				perror( "accept" );
				exit( -1 );
			}
			numRequests++;
			//fprintf(stderr, "Slave Socket: %d\n", slaveSocket);
			pthread_t tid;
			pthread_attr_t attr;
			pthread_attr_init(&attr);
			pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

			pthread_create(&tid, &attr, (void * (*)(void *))processRequestThread, (void *)&slaveSocket);
		}
	} else if (pool_based) {
		pthread_mutex_init(&mutex, NULL);

		pthread_t tid[5];
		pthread_attr_t attr;
		pthread_attr_init(&attr);
		pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

		for (int i = 0; i < 5; i++) {
			pthread_create(&tid[i], &attr, (void *(*)(void *))poolSlave, (void *)&masterSocket);
		}
		while(1) {
		}
	} else {	
		while(1) {
			 //Accept new TCP connection
			struct sockaddr_in clientIPAddress;
			int alen = sizeof( clientIPAddress );
			int slaveSocket = accept( masterSocket, 
				(struct sockaddr *)&clientIPAddress, (socklen_t*)&alen);

			if ( slaveSocket < 0 ) {
				perror( "accept" );
				exit( -1 );
			}
			numRequests++;

			processRequest(slaveSocket);

			//Close TCP connection
			shutdown( slaveSocket, SHUT_RDWR );
			close( slaveSocket );
		}
	}
}

void processRequestThread(int* slaveSocket) {
	int sock = slaveSocket[0];
	processRequest(sock);
	//Close TCP connection
	shutdown( sock, SHUT_RDWR );
	close( sock );
}

void poolSlave(int* socket) {
	int sock = socket[0];

	while(1) {
		 //Accept new TCP connection
		pthread_mutex_lock(&mutex);
    	struct sockaddr_in clientIPAddress;
    	int alen = sizeof( clientIPAddress );
    	int slaveSocket = accept( sock, 
			(struct sockaddr *)&clientIPAddress, (socklen_t*)&alen);
		numRequests++;
		pthread_mutex_unlock(&mutex);
	    if ( slaveSocket < 0 ) {
	    	perror( "accept" );
	    	exit( -1 );
	    }
		processRequest(slaveSocket);
		//Close TCP connection
		shutdown( slaveSocket, SHUT_RDWR );
	    close( slaveSocket );
	}

}

void processRequest(int slaveSocket) {

	const int MaxSize = 1024;
	char curr_string[ MaxSize + 1 ];
	int length = 0;
	
	int n;
	int gotGet = 0;
	int gotDoc = 0;
	char *docpath;

	unsigned char newChar;
	unsigned char oldChar;

	//read the HTTP header
	while ((n = read(slaveSocket, &newChar, sizeof(newChar))) > 0) {
		
		if (newChar == ' ') {
			if (gotGet == 0) {
				gotGet = 1;
			} else if (gotDoc == 0) {
				gotDoc = 1;
				curr_string[length] = 0;
				docpath = strdup(curr_string);
			}
		} else if ((newChar == '\n') && (oldChar == '\r')) {
			oldChar = newChar;
			break;
		} else if (gotGet > 0) {
			oldChar = newChar;
			length++;
			curr_string[length - 1] = newChar;
		}
	}
	//read the remaining header and ignore it
	unsigned char older = '\0';
	unsigned char oldest = '\0';
	while ((n = read(slaveSocket, &newChar, sizeof(newChar))) > 0) {
		//ignore it
		if ((oldChar == '\r') && (newChar == '\n') && (oldest == '\r') && (older == '\n')) {
			break;
		} else {
			if (older == '\0') {
				older = oldChar;
				oldChar = newChar;
			} else {
				oldest = older;
				older = oldChar;
				oldChar = newChar;
			}
		}
	}

	char* args;
	if ((args = getArgs(docpath)) != NULL) {
		processFilePath(slaveSocket, removeArgs(docpath), args);
	} else {
		processFilePath(slaveSocket, docpath, NULL);
	}
}

void processFilePath(int slaveSocket, char *docpath, char *args) {
	time_t t1;
	time(&t1);
	const int MaxSize = 1024;
	//map the document path to the real file
	char *cwd = (char *)malloc(256*sizeof(char));
	cwd = getcwd(cwd, 256);
	char *file_path = (char *)malloc((MaxSize + 1)*sizeof(char));
	strcpy(file_path, cwd);

	if ((!strcmp(docpath, "/")) && (strlen(docpath) == 1)) {
		strcat(file_path, "/http-root-dir/htdocs/index.html");
	} else if (!strncmp(docpath, "/icons", strlen("/icons"))) {
		strcat(file_path, "/http-root-dir");
		strcat(file_path, docpath);
	} else if (!strncmp(docpath, "/htdocs", strlen("/htdocs"))) {
		strcat(file_path, "/http-root-dir");
		strcat(file_path, docpath);
	} else if (!strncmp(docpath, "/cgi-bin", strlen("/cgi-bin"))) {
		strcat(file_path, "/http-root-dir");
		strcat(file_path, docpath);
	} else {
		strcat(file_path, "/http-root-dir/htdocs");
		strcat(file_path, docpath);
	}

	//expand filepath	
	char *real_path = (char *)malloc((MaxSize + 1)*sizeof(char));
	file_path = realpath(file_path, real_path);

	if (strlen(real_path) < (strlen(cwd) + strlen("/http-root-dir"))) {
		//send a 403 error access denied or whatever
		send403(slaveSocket);
		return;
	} else {
		addToLog(slaveSocket, real_path);
		//determine content type
		char contentType[ MaxSize + 1 ];
		if (endsWith(real_path, ".html") || endsWith(real_path, ".html/")) {
			strcpy(contentType, "text/html");
		} else if (endsWith(real_path, ".gif") || endsWith(real_path, ".gif/")) {
			strcpy(contentType, "image/gif");
		} else if (endsWith(real_path, ".svg") || endsWith(real_path, ".svg/")) {
			strcpy(contentType, "image/svg+xml");
		} else {
			strcpy(contentType, "text/plain");
		}

		const char* cgiBin = "/cgi-bin/";
		if (!strncmp(docpath, cgiBin, strlen(cgiBin))) {
			processCGIBin(slaveSocket, docpath, real_path, args);
		} else {
			DIR* d = opendir(real_path);
			if ( d != NULL) {
				sendDir(slaveSocket, d, docpath, real_path, contentType, args);
			} else {
				//open the file
				int fd = open(real_path, O_RDONLY);
				if (fd < 0) {
					//if open() fails, send a 404
					if (endsWith(real_path, "/stats")) {
						sendStats(slaveSocket, real_path);
					} else {
						send404(slaveSocket, contentType);
						return;
					}
				} else {
					//send http reply header
					send200(slaveSocket, contentType, fd);
				}
			}
		}
	}
	time_t t2;
	time(&t2);
	double dt = difftime(t2, t1);
	compareTimes(dt, docpath);
}

void processCGIBin(int slaveSocket, char* docpath, char* real_path, char* args) {

	const char *message = "HTTP/1.0 200 Document follows\r\n"
						"Server: MyAwesomeServer\r\n";
	write(slaveSocket, message, strlen(message));

	if (!endsWith(real_path, ".so")) {

		pid_t slave = fork();
		if (slave == 0) {
			if (args != NULL) {
				setenv("REQUEST_METHOD", "GET", 1);
				setenv("QUERY_STRING", args, 1);
			}

			dup2(slaveSocket, 1);
			close(slaveSocket);

			if (args != NULL) {
				execl(real_path, args,0,(char*)0);
			} else {
				execl(real_path, NULL,0,(char*)0);
			}
			exit(0);
		}
		waitpid(slave, NULL, 0);
		shutdown( slaveSocket, SHUT_RDWR );
		close( slaveSocket );
	} else {
		runLoadable(slaveSocket, real_path, args);
	}
}

void runLoadable(int slaveSocket, char *docpath, char *args) {

	void *dlo = dlopen(docpath, RTLD_LAZY);
	if (!dlo) {
        fprintf(stderr, "%s\n", dlerror());
		return;
    }
	dlerror();
	
	void (*dls)(int, char *);
	*(void **)(&dls) = dlsym(dlo, "httprun");
	char *error;
	if ((error = dlerror()) != NULL)  {
        fprintf(stderr, "%s\n", error);
		return;
    }
	(*dls)(slaveSocket, args);
	dlclose(dlo);
}

char *getScript(char *docpath) {
	char *dp;
	if (getArgs(docpath) != NULL) {
		dp = removeArgs(docpath);
	} else {
		dp = docpath;
	}
	char *script;
	int last = -1;

	for (int i = 0; i < strlen(dp); i++) {
		if (dp[i] == '/') {
			last = i;
		}
	}

	script = (char*)malloc((strlen(dp) - last)*sizeof(char));
	int pos = 0;	
	for (int j = last+1; j < strlen(dp); j++) {
		script[pos++] = dp[j];
	}
	script[pos] = '\0';
	
	return script;
}

int endsWith(char *first, const char *second) {

	int len1 = strlen(first);
	int len2 = strlen(second);
	int pos = len1 - len2;

	for (int i = 0; i < len2; i++) {
		char c1 = first[pos + i];
		char c2 = second[i];
		if (c1 != c2) {
			return 0;
		}
	}
	return 1;
}

void send404(int slaveSocket, char *contentType) {
	//Write the response header to TCP connection.
	//Frame the appropriate response header depending on whether the URL requested is found on the 		
	//server or not.
	const char *notFound = "File not Found";
	const char *message = "HTTP/1.0 404FileNotFound\r\n"
							"Server: MyAwesomeServer\r\n"
							"Content-type: ";
	write(slaveSocket, message, strlen(message));
	write(slaveSocket, contentType, strlen(contentType));
	write(slaveSocket, "\r\n", 2);
	write(slaveSocket, "\r\n", 2);
	write(slaveSocket, notFound, strlen(notFound));
}

void send403(int slaveSocket) {
	//Write the response header to TCP connection.
	const char *forbidden = "Access Denied";
	const char *message = "HTTP/1.0 403Forbidden\r\n"
							"Server: MyAwesomeServer\r\n\r\n";
	write(slaveSocket, message, strlen(message));
	write(slaveSocket, forbidden, strlen(forbidden));
}

void send200(int slaveSocket, char *contentType, int fd) {
	//Write the response header to TCP connection.
	//Write requested document (if found) to TCP connection.
	const char *message = "HTTP/1.0 200 Document follows\r\n"
							"Server: MyAwesomeServer\r\n"
							"Content-type: ";
	write(slaveSocket, message, strlen(message));
	write(slaveSocket, contentType, strlen(contentType));
	write(slaveSocket, "\r\n", 2);
	write(slaveSocket, "\r\n", 2);

	char ch;
	int count;
	while ((count = read(fd, &ch, 1)) > 0) {
		write(slaveSocket, &ch, 1);
	}
}

void sendDir(int slaveSocket, DIR * d, char* docpath, char* real_path, char *contentType, char *args) {
	const char *message = "HTTP/1.0 200 Document follows\r\n"
							"Server: MyAwesomeServer\r\n"
							"Content-type: text/html";
	write(slaveSocket, message, strlen(message));
	write(slaveSocket, "\r\n", 2);
	write(slaveSocket, "\r\n", 2);

	const char *message2a = "<html>\n <head>\n  <title>Index of ";
	const char *message2b = "</title>\n </head>\n <body>\n<h1>Index of ";
	const char *message2c = "</h1>\n<table><tr>";

	write(slaveSocket, message2a, strlen(message2a));
	write(slaveSocket, docpath, strlen(docpath));
	write(slaveSocket, message2b, strlen(message2b));
	write(slaveSocket, docpath, strlen(docpath));
	write(slaveSocket, message2c, strlen(message2c));

	const char *message2d;

	if (args == NULL){
		message2d = "<tr><th><img src=\"/icons/blue_ball.gif\" alt=\"[ICO]\">"
						"</th><th><a href=\"?C=N;O=D\">Name</a></th><th><a href=\"?C=M;"
						"O=A\">Last modified</a></th><th><a href=\"?C=S;O=A\">Size</a>"
						"</th><th><a href=\"?C=D;O=A\">Description</a></th></tr>"
						"<tr><th colspan=\"5\"><hr></th></tr>";
	} else if (args[2] == 'N') {
		if (args[6] == 'A') {
			message2d = "<tr><th><img src=\"/icons/blue_ball.gif\" alt=\"[ICO]\">"
						"</th><th><a href=\"?C=N;O=D\">Name</a></th><th><a href=\"?C=M;"
						"O=A\">Last modified</a></th><th><a href=\"?C=S;O=A\">Size</a>"
						"</th><th><a href=\"?C=D;O=A\">Description</a></th></tr>"
						"<tr><th colspan=\"5\"><hr></th></tr>";
		} else {
			message2d = "<tr><th><img src=\"/icons/blue_ball.gif\" alt=\"[ICO]\">"
						"</th><th><a href=\"?C=N;O=A\">Name</a></th><th><a href=\"?C=M;"
						"O=A\">Last modified</a></th><th><a href=\"?C=S;O=A\">Size</a>"
						"</th><th><a href=\"?C=D;O=A\">Description</a></th></tr>"
						"<tr><th colspan=\"5\"><hr></th></tr>";
		}
	} else if (args[2] == 'M') {
		if (args[6] == 'A') {
			message2d = "<tr><th><img src=\"/icons/blue_ball.gif\" alt=\"[ICO]\">"
						"</th><th><a href=\"?C=N;O=A\">Name</a></th><th><a href=\"?C=M;"
						"O=D\">Last modified</a></th><th><a href=\"?C=S;O=A\">Size</a>"
						"</th><th><a href=\"?C=D;O=A\">Description</a></th></tr>"
						"<tr><th colspan=\"5\"><hr></th></tr>";
		} else {
			message2d = "<tr><th><img src=\"/icons/blue_ball.gif\" alt=\"[ICO]\">"
						"</th><th><a href=\"?C=N;O=A\">Name</a></th><th><a href=\"?C=M;"
						"O=A\">Last modified</a></th><th><a href=\"?C=S;O=A\">Size</a>"
						"</th><th><a href=\"?C=D;O=A\">Description</a></th></tr>"
						"<tr><th colspan=\"5\"><hr></th></tr>";
		}
	} else if (args[2] == 'S') {
		if (args[6] == 'A') {
			message2d = "<tr><th><img src=\"/icons/blue_ball.gif\" alt=\"[ICO]\">"
						"</th><th><a href=\"?C=N;O=A\">Name</a></th><th><a href=\"?C=M;"
						"O=A\">Last modified</a></th><th><a href=\"?C=S;O=D\">Size</a>"
						"</th><th><a href=\"?C=D;O=A\">Description</a></th></tr>"
						"<tr><th colspan=\"5\"><hr></th></tr>";
		} else {
			message2d = "<tr><th><img src=\"/icons/blue_ball.gif\" alt=\"[ICO]\">"
						"</th><th><a href=\"?C=N;O=A\">Name</a></th><th><a href=\"?C=M;"
						"O=A\">Last modified</a></th><th><a href=\"?C=S;O=A\">Size</a>"
						"</th><th><a href=\"?C=D;O=A\">Description</a></th></tr>"
						"<tr><th colspan=\"5\"><hr></th></tr>";
		}
	} else {
		message2d = "<tr><th><img src=\"/icons/blue_ball.gif\" alt=\"[ICO]\">"
						"</th><th><a href=\"?C=N;O=D\">Name</a></th><th><a href=\"?C=M;"
						"O=A\">Last modified</a></th><th><a href=\"?C=S;O=A\">Size</a>"
						"</th><th><a href=\"?C=D;O=A\">Description</a></th></tr>"
						"<tr><th colspan=\"5\"><hr></th></tr>";
	}
	const char *message2e = "<tr><td valign=\"top\"><img src=\"/icons/menu.gif\""
						" alt=\"[DIR]\"></td><td><a href=\"";
	const char *message2f = getParentDirectory(docpath);
	const char *message2g = "\">Parent Directory</a>       </td><td>&nbsp;"
						"</td><td align=\"right\">  - </td><td>&nbsp;</td></tr>";

	write(slaveSocket, message2d, strlen(message2d));
	write(slaveSocket, message2e, strlen(message2e));
	write(slaveSocket, message2f, strlen(message2f));
	write(slaveSocket, message2g, strlen(message2g));
	
	struct FileStats **files = (struct FileStats **)malloc(100*sizeof(struct FileStats));
	struct dirent *ch;
	int numFiles = 0;
	while ((ch = readdir(d)) != NULL) {
		struct FileStats *f = (struct FileStats*)malloc(sizeof(struct FileStats));
		f->dir = ch;

		char *path = (char *)malloc(1024*sizeof(char));
		strcpy(path, real_path);
		if (!endsWith(path, "/")) {
			strcat(path, "/");
		}
		strcat(path, ch->d_name);
		f->p = path;

		char *dpath = (char *)malloc(1024*sizeof(char));
		strcpy(dpath, docpath);
		if (!endsWith(dpath, "/")) {
			strcat(dpath, "/");
		}
		strcat(dpath, ch->d_name);
		f->dp = dpath;

		struct stat buf;
		stat(path, &buf);
		f->b = buf;

		files[numFiles++] = f;
	}

	sortFiles(files, numFiles, args);

	for (int i = 0; i < numFiles; i++) {
		

		const char *message3a;
		if (files[i]->dir->d_type == DT_DIR) {
			message3a = "<tr><td valign=\"top\"><img src=\"/icons/menu.gif\""
						" alt=\"[DIR]\"></td><td><a href=\"";
		} else {
			if (endsWith(files[i]->p, ".html") || endsWith(files[i]->p, ".html/")) {
				message3a = "<tr><td valign=\"top\"><img src=\"/icons/text.gif\""
						" alt=\"[   ]\"></td><td><a href=\"";
			} else if (endsWith(files[i]->p, ".gif") || endsWith(files[i]->p, ".gif/")) {
				message3a = "<tr><td valign=\"top\"><img src=\"/icons/image.gif\""
						" alt=\"[   ]\"></td><td><a href=\"";
			} else if (endsWith(files[i]->p, ".svg") || endsWith(files[i]->p, ".svg/")) {
				message3a = "<tr><td valign=\"top\"><img src=\"/icons/image.gif\""
						" alt=\"[   ]\"></td><td><a href=\"";
			} else if (endsWith(files[i]->p, ".xbm") || endsWith(files[i]->p, ".xbm/")) {
				message3a = "<tr><td valign=\"top\"><img src=\"/icons/image.gif\""
						" alt=\"[   ]\"></td><td><a href=\"";
			} else {
				message3a = "<tr><td valign=\"top\"><img src=\"/icons/unknown.gif\""
						" alt=\"[   ]\"></td><td><a href=\"";
			}
		}
		write(slaveSocket, message3a, strlen(message3a));
		write(slaveSocket, files[i]->dp, strlen(files[i]->dp));
		write(slaveSocket, "\">", strlen("\">"));
		write(slaveSocket, files[i]->dir->d_name, strlen(files[i]->dir->d_name));
		
		

		const char *message3b = "</a>               </td><td align=\"right\">";
		const char *message3c = ctime(&(files[i]->b.st_mtime));	//date modified
		const char *message3d = "</td><td align=\"right\">";
		const char *message3e = sizeToString(files[i]->b.st_size);
		const char *message3f = "</td><td>&nbsp;</td></tr>\n";
		
		write(slaveSocket, message3b, strlen(message3b));
		write(slaveSocket, message3c, strlen(message3c));
		write(slaveSocket, message3d, strlen(message3d));
		write(slaveSocket, message3e, strlen(message3e));
		write(slaveSocket, message3f, strlen(message3f));
	}

	const char *message4a = "<tr><th colspan=\"5\"><hr></th></tr>\n";
	const char *message4b = "</table>\n<address>Apache/2.2.24 (Unix) mod_ssl/2.2.24"
 				"OpenSSL/0.9.8ze Server at www.cs.purdue.edu Port ";
	const char *message4c = numToString(port);
	const char *message4d = "</address>\n</body></html>\n";
	
	write(slaveSocket, message4a, strlen(message4a));
	write(slaveSocket, message4b, strlen(message4b));
	write(slaveSocket, message4c, strlen(message4c));
	write(slaveSocket, message4d, strlen(message4d));
}

void sortFiles(struct FileStats **files, int numFiles, char *args) {
	if (args == NULL) {
		qsort((void**)files, numFiles, sizeof(struct dirent*), compareNA);
	} else if (args[6] == 'A') {
		if (args[2] == 'M') {
			qsort(files, numFiles, sizeof(struct dirent*), compareMA);
		} else if (args[2] == 'S') {
			qsort(files, numFiles, sizeof(struct dirent*), compareSA);
		} else {
			qsort(files, numFiles, sizeof(struct dirent*), compareNA);
		}
	} else {
		if (args[2] == 'M') {
			qsort(files, numFiles, sizeof(struct dirent*), compareMD);
		} else if (args[2] == 'S') {
			qsort(files, numFiles, sizeof(struct dirent*), compareSD);
		} else {
			qsort(files, numFiles, sizeof(struct dirent*), compareND);
		}
	}
}

int compareNA(const void* a, const void* b) {
    struct FileStats *ia = *(struct FileStats **)a;
    struct FileStats *ib = *(struct FileStats **)b;
    return strcmp(ia->dir->d_name, ib->dir->d_name);
}

int compareMA(const void* a, const void* b) {
    struct FileStats *ia = *(struct FileStats **)a;
    struct FileStats *ib = *(struct FileStats **)b;
	return strcmp(ctime(&(ia->b.st_mtime)),ctime(&(ib->b.st_mtime)));
}

int compareSA(const void* a, const void* b) {
    struct FileStats *ia = *(struct FileStats **)a;
    struct FileStats *ib = *(struct FileStats **)b;
	return ia->b.st_size - ib->b.st_size;
}

int compareND(const void* a, const void* b) {
	struct FileStats *ia = *(struct FileStats **)a;
    struct FileStats *ib = *(struct FileStats **)b;
    return strcmp(ib->dir->d_name, ia->dir->d_name);
}

int compareMD(const void* a, const void* b) {
    struct FileStats *ia = *(struct FileStats **)a;
    struct FileStats *ib = *(struct FileStats **)b;
	return strcmp(ctime(&(ib->b.st_mtime)),ctime(&(ia->b.st_mtime)));
}

int compareSD(const void* a, const void* b) {
    struct FileStats *ia = *(struct FileStats **)a;
    struct FileStats *ib = *(struct FileStats **)b;
	return ib->b.st_size - ia->b.st_size;
}

char *numToString(int num) {
		char buf[1024] = {0x0};
		sprintf(buf, "%d", num);
		char *str = strdup(buf);
		return str;
}

char *sizeToString(int num) {
	float f = num/1000;

	if (f < 1) {
		return numToString(num);
	} else if (f < 1000) {
		char buf[10] = {0x0};
		sprintf(buf, "%.1f", f);
		strcat(buf, "K");
		char *str = strdup(buf);
		return str;
	} else if ((f = f/1000) < 1000) {
		char buf[10] = {0x0};
		sprintf(buf, "%f", f);
		strcat(buf, "M");
		char *str = strdup(buf);
		return str;
	} else {
		f = f/1000;
		char buf[10] = {0x0};
		sprintf(buf, "%f", f);
		strcat(buf, "G");
		char *str = strdup(buf);
		return str;
	}
}

char *getParentDirectory(char *child) {

	int childLen = strlen(child);

	int count = 1;
	while (child[childLen - count - 1] != '/') {
		count++;
	}

	char *parent = (char*)malloc((childLen-count+1)*sizeof(char));
	for(int i = 0; i < childLen - count; i++) {
		parent[i] = child[i];
	}
	parent[childLen-count] = '\0';
	return parent;
}

char *getArgs(char *dir) {
	int hasArgs = 0;
	char *args;
	int pos = 0;

	for (int i = 0; i < strlen(dir); i++) {
		if (dir[i] == '?') {
			hasArgs = 1;
			args = (char*)malloc((strlen(dir)-i-1)*sizeof(char));
		} else if (hasArgs == 1) {
			args[pos++] = dir[i];
		}
	}
	if (!hasArgs) {
		return NULL;
	} else {
		return args;
	}
}

char *removeArgs(char *dir) {
		
	char *removed = (char *)malloc(strlen(dir)*sizeof(char));

	int pos = 0;
	int last = -1;
	while (dir[pos] != '?') {
		removed[pos] = dir[pos];
		if (dir[pos] == '/') {
			last = pos;
		}
		pos++;
	}

	return removed;
}

void addToLog(int sock, char *directory) {
	char *cwd = (char *)malloc(256*sizeof(char));
	cwd = getcwd(cwd, 256);
	char *file_path = (char *)malloc((1024 + 1)*sizeof(char));
	strcpy(file_path, cwd);
	strcat(file_path, "/http-root-dir/htdocs/logs");

	struct sockaddr_in sad;
	int sadLen = sizeof(sad);
	getsockname(sock, (struct sockaddr*) &sad, (socklen_t *) &sadLen);
	char *ipAddr = inet_ntoa(sad.sin_addr);

	char *newLog = (char*)malloc(1024*sizeof(char));
	strcpy(newLog, "Host: ");
	strcat(newLog, ipAddr);
	strcat(newLog, "\tDirectory: ");
	strcat(newLog, directory);
	strcat(newLog, "\n");

	int fd = open(file_path, O_WRONLY | O_APPEND, 0666);
	//mmap (newLog, sizeof(newLog), PROT_READ, MAP_SHARED, fd, 0);
	//pthread_mutex_lock(&wmutex);
	write(fd, newLog, strlen(newLog));
	//pthread_mutex_unlock(&wmutex);*/
}

void sendStats(int slaveSocket, char *real_path) {
	const char *message = "HTTP/1.0 200 Document follows\r\n"
							"Server: MyAwesomeServer\r\n"
							"Content-type: text/html";
	write(slaveSocket, message, strlen(message));
	write(slaveSocket, "\r\n", 2);
	write(slaveSocket, "\r\n", 2);

	const char *message2a = "<html>\n <head>\n  <title>Statistics of MyAwesomeServer";
	const char *message2b = "</title>\n </head>\n <body>\n<h1>Statistics of MyAwesomeServer</h1>\n";

	write(slaveSocket, message2a, strlen(message2a));
	write(slaveSocket, message2b, strlen(message2b));


	const char *message3a = "<h3>Creator of this project:</h3>";
	const char *message3b = "<h4>Emma Caraher</h4>";
	const char *message3c = "Studying at Purdue University\n<br>";
	const char *message3d = "Project made for CS 252: Systems Programming\n<br>";

	write(slaveSocket, message3a, strlen(message3a));
	write(slaveSocket, message3b, strlen(message3b));
	write(slaveSocket, message3c, strlen(message3c));
	write(slaveSocket, message3d, strlen(message3d));

	const char *message4a = "<h3>Time since the server opened:</h3><h4>";
	time_t endTime;
	time(&endTime);
	double dt = difftime(endTime, startTime);
	const char *message4b = timeToString(dt);
	const char *message4c = "</h4>";

	write(slaveSocket, message4a, strlen(message4a));
	write(slaveSocket, message4b, strlen(message4b));
	write(slaveSocket, message4c, strlen(message4c));

	const char *message5a = "<h3>Number of Requests Since Server Started</h3><h4>";
	char *message5b = numToString(numRequests);
	
	write(slaveSocket, message5a, strlen(message5a));
	write(slaveSocket, message5b, strlen(message5b));
	write(slaveSocket, message4c, strlen(message4c));

	//compareTimes(dt, real_path);
	if (minTime != -1) {
		const char *message6a = "<h3>Minimum Service Time Since Server Started</h3>Running: ";
		const char *message6b = "took: \n";
	
		write(slaveSocket, message6a, strlen(message6a));
		write(slaveSocket, minChar, strlen(minChar));
		write(slaveSocket, message6b, strlen(message6b));
		write(slaveSocket, timeToString(minTime), strlen(timeToString(minTime)));

		const char *message7a = "<h3>Maximum Service Time Since Server Started</h3>Running: ";
		const char *message7b = "took: \n";
	
		write(slaveSocket, message7a, strlen(message7a));
		write(slaveSocket, maxChar, strlen(maxChar));
		write(slaveSocket, message7b, strlen(message7b));
		write(slaveSocket, timeToString(maxTime), strlen(timeToString(maxTime)));
	}
}

void compareTimes(double time, char* docpath) {
	if (minTime == -1) {
		minTime = time;
		minChar = strdup(docpath);
		maxTime = time;
		maxChar = strdup(docpath);
	} else if (time < minTime) {
		minChar = strdup(docpath);
		minTime = time;
	} else if (time > maxTime) {
		maxTime = time;
		maxChar = strdup(docpath);
	}
}

char *timeToString(double num) {

	if (num < 60) {
		char buf[10] = {0x0};
		sprintf(buf, "%.0f", num);
		strcat(buf, " seconds");
		char *str = strdup(buf);
		return str;
	} else if ((num = num/60) < 60) {
		char buf[10] = {0x0};
		sprintf(buf, "%.1f", num);
		strcat(buf, " minutes");
		char *str = strdup(buf);
		return str;
	} else {
		num = num/60;
		char buf[10] = {0x0};
		sprintf(buf, "%.1f", num);
		strcat(buf, " hours");
		char *str = strdup(buf);
		return str;
	}
}








