#include "NetworkSocket.h"
#include <errno.h>
#include "CommandExecutionEngine.h"

void* get_internet_addr(struct sockaddr* sa)
{
	if (sa->sa_family == AF_INET)
		return &(((struct sockaddr_in*)sa)->sin_addr);
	return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

int recvtimeout(int s, char *buf, int len, int timeout)
{
	fd_set fds;
	int n;
	struct timeval tv;
	FD_ZERO(&fds);
	FD_SET(s, &fds);
	tv.tv_sec = timeout;
	tv.tv_usec = 0;
	n = select(s + 1, &fds, NULL, NULL, &tv);
	if (n == 0) return -2; // timeout!
	if (n == -1) return -1; // error
	return recv(s, buf, len, 0);
}

bool NetworkSocket::SendMessage(string ip, string message)
{
	cout << ip << endl;
	int socket_fd;
	sockaddr_in local, remote;
	
	if ((socket_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1)
	{
		cout << "Error creating socket for sending!" << endl;
		return false;
	}
	
	int tempOne = 1;
	if (setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &tempOne, sizeof(int)) == -1)
	{
		cout << "Error on setting socket options for sending!" << endl;
		cout << strerror(errno) << endl;
		return false;
	}
	
	remote.sin_family = AF_INET; //IPV4
	remote.sin_port = htons(SEND_PORT); //Host To Network byte order van port
	if ((inet_pton(AF_INET, ip.c_str(), &remote.sin_addr)) == -1)
	{
		cout << "Invalid IP Adress!" << endl;
		return false;
	}
	bzero(&remote.sin_zero, 8); //zero'en van de laatste member van de struct
	
	int len = sizeof(sockaddr_in);
		
	
	if ((connect(socket_fd, (sockaddr*)&remote, sizeof(sockaddr_in))))
	{
		cout << "Error connecting to receiver of message!" << endl;
		return false;
	}
	cout << "Connected to pc to send msg to" << endl;
	send(socket_fd, message.c_str(),message.length(),0);
	
	close(socket_fd);
}


NetworkSocket::NetworkSocket(int port, CommandExecutionEngine& execEngine) : m_Port(port), m_ExecEngine(execEngine)
{
}


NetworkSocket::~NetworkSocket()
{
	if (m_Thread != nullptr)
	{
		StopSocketServer();
	}
	
}

auto NetworkSocket::RunSocketServer() -> void
{
	m_Thread = new thread(&NetworkSocket::RunServer, this);
}

auto NetworkSocket::StopSocketServer() -> void
{
	if (m_Thread != nullptr)
	{
		m_Stop = false;
		shutdown(m_Sock_fd, SHUT_RDWR);
		m_Thread->join();
		delete m_Thread;
		m_Thread = nullptr;
	}
}

auto NetworkSocket::RunServer() -> void
{
	fd_set master;
	fd_set read_fds;
	int fdmax; //max file descs
		
	FD_ZERO(&master);
	FD_ZERO(&read_fds);
		
	cout << "Starting socket server" << endl;
	//Current host printen
	cout << string([]() -> string{
		char buf[128];
		if (gethostname(buf, 128) == -1)
		{
			cout << "Error getting local hostname!" << endl;
			cout << strerror(errno) << endl;
			return string("\nNon-Fatal error occurred. The server will continue");
		}
		string rtr(buf);
		rtr = "Local hostname: " + rtr;
		return rtr;
	}()).c_str() << endl;
	
	//Variabelen
	
	int client_socket_fd; // fd = file descriptor. Immers alles in linux is een file. Zo ook een socket. //Listenen op m_Sock_Listening
	struct addrinfo info, *serverinfo;
	struct sockaddr_storage their_addr; //Client information
	socklen_t sin_size;
	
	
	//Addressinformatie verkrijgen in een linked list van addrinfo's
	memset(&info, 0, sizeof(info));
	info.ai_family = AF_UNSPEC; //IPv4
	info.ai_socktype = SOCK_STREAM; //TCP Socket
	info.ai_flags = AI_PASSIVE; //Use local IP adress
	if ((getaddrinfo(NULL, std::to_string(m_Port).c_str(), &info, &serverinfo)) != 0)
	{
		cout << "Error on getting addres information: " << endl;
		cout << strerror(errno) << endl;
		return;
	}
	
	//Eerst beschikbare informatie zoeken om te gebruiken.
	for (struct addrinfo* temp = serverinfo; temp != NULL; temp = temp->ai_next)
	{
		//Socket openen
		if ((m_Sock_fd = socket(temp->ai_family, temp->ai_socktype, temp->ai_protocol)) == -1)
		{
			cout << "Error on opening socket. Trying next" << endl;
			cout << strerror(errno) << endl;
			continue; //Immers we proberen de volgende
		}
		
		//Socket opties instellen ivm hergebruik van socket
		int tempOne = 1;
		if (setsockopt(m_Sock_fd, SOL_SOCKET, SO_REUSEADDR, &tempOne, sizeof(int)) == -1)
		{
			cout << "Error on setting socket options!" << endl;
			cout << strerror(errno) << endl;
			return;
		}
		
		//Socket binden
		if (bind(m_Sock_fd, temp->ai_addr, temp->ai_addrlen) < 0)
		{
			cout << "Error on binding to the port!" << endl;
			cout << strerror(errno) << endl;
			return;
		}
		break;
	}	
	freeaddrinfo(serverinfo); //rotzooi opruimen
	
	if (listen(m_Sock_fd, MAX_CONNECTIONS) == -1)
	{
		cout << "Error on listening on the port!" << endl;
		cout << strerror(errno) << endl;
		return;
	}
	
	cout << "Server listening for new connections" << endl;
	
	if (fcntl(m_Sock_fd, F_SETFL, fcntl(m_Sock_fd, F_GETFL) | O_NONBLOCK) < 0) {
		cout << "Error putting listening socket in non block mode!" << endl;
	}
	
	FD_SET(m_Sock_fd, &master);
	fdmax = m_Sock_fd;
	
	while (m_Stop != false)
	{
		read_fds = master;
		if (select(fdmax + 1, &read_fds, NULL, NULL, NULL) == -1)	
		{
			cout << "Error on select()!" << endl;
			cout << strerror(errno) << endl;
			return;
		}
		
		for (int i = 0; i <= fdmax; ++i)
		{
			if (FD_ISSET(i, &read_fds)) //Er is een socket ready!
			{
				if (i == m_Sock_fd) //als dit de luisterende socket is, is er een nieuwe client
				{
					char ipBuf[INET6_ADDRSTRLEN];
					sin_size = sizeof(their_addr);
					client_socket_fd = accept(m_Sock_fd, (struct sockaddr*)&their_addr, &sin_size);
					if (client_socket_fd == -1)
					{
						cout << "Some error occurred on accepting. Server will continue" << endl;
						cout << strerror(errno) << endl;
					}
					else
					{
						FD_SET(client_socket_fd, &master); // client aan de master set toevoegen
						if (client_socket_fd > fdmax)
							fdmax = client_socket_fd;
					}	
					if (fcntl(client_socket_fd, F_SETFL, fcntl(client_socket_fd, F_GETFL) | O_NONBLOCK) < 0) {
						cout << "Error putting client socket in non block mode!" << endl;
					}
					inet_ntop(their_addr.ss_family, get_internet_addr((struct sockaddr*)&their_addr), ipBuf, sizeof(ipBuf));
					cout << "A new client connected from " << string(ipBuf) << endl;
					//Nieuwe client vanaf hier processen
				}
				else
				{
					//Inkomende data van een client
					char buf[MAX_REPLY_BUFFER_SIZE];
					int byteCount;
					if ((byteCount = recvtimeout(i,buf,sizeof(buf), 1)) <= 0)
					{
						if (byteCount == 0)
							cout << "A socket hung up: " << i << endl;
							//Connectie is gesloten door client
						else
						{					
							if (byteCount == -2)
							{
								cout << "Client timed out: " << i << endl;
							}
							else
								cout << "Error on recv()!" << endl;
						}
						close(i);
						FD_CLR(i, &master); //uit de set halen
					}
					else
					{
						buf[byteCount] = '\0';
						//Got data from client!
						cout << "Received reply: " << string(buf) << endl;
						char ipBuf[INET6_ADDRSTRLEN];
						inet_ntop(their_addr.ss_family, get_internet_addr((struct sockaddr*)&their_addr), ipBuf, sizeof(ipBuf));	
						m_ExecEngine.Execute(string(buf), string(ipBuf));				
					}
				}
			}			
		}	
	}
	close(m_Sock_fd);
	cout << "Server gracefully shutdown" << endl;
}