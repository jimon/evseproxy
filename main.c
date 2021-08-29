#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef _WIN32
	#include <winsock2.h>
	typedef SOCKET socket_t;
#else
	#include <sys/socket.h>
	#include <netinet/in.h>
	#include <arpa/inet.h>
	#include <fcntl.h>
	#include <signal.h>
	#include <errno.h>
	#include <unistd.h>
	typedef int socket_t;
#endif

static bool run = true;

static void wait_a_bit(int ms);
static void sock_init(void);
static void sock_deinit(void);
static void sock_setnonblock(socket_t* s);
static bool sock_wouldblock(void);
static void sock_close(socket_t* s);
static void ctrl_c_setup(void);

static void sock_ensure(socket_t* s, const char* ip, int port);

#ifdef _WIN32

static void wait_a_bit(int ms)
{
	Sleep(ms);
}

static void sock_init(void)
{
	WSADATA wsa;
	if(WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
	{
		//printf("WSAStartup failed\n");
		//fflush(stdout);
		exit(1);
	}
}

static void sock_deinit(void)
{
	WSACleanup();
}

static void sock_setnonblock(socket_t* s)
{
	if (s == NULL)
		return;
	u_long mode = 1;
	ioctlsocket(*s, FIONBIO, &mode);
}

static bool sock_wouldblock(void)
{
	return WSAGetLastError() == WSAEWOULDBLOCK;
}

static void sock_close(socket_t* s)
{
	if (s == NULL)
		return;
	//shutdown(*s, SD_BOTH);
	closesocket(*s);
	*s = INVALID_SOCKET;
}

static BOOL WINAPI ctrl_c_handler(DWORD type)
{
	if(type != CTRL_C_EVENT)
		return FALSE;

	run = false;
	return TRUE;
}

static void ctrl_c_setup(void)
{
	if(SetConsoleCtrlHandler(ctrl_c_handler, TRUE) == FALSE)
	{
		//printf("SetConsoleCtrlHandler failed\n");
		//fflush(stdout);
		exit(1);
	}
}

#else

#define INVALID_SOCKET -1
#define SOCKET_ERROR -1

static void wait_a_bit(int ms)
{
	usleep(ms * 1000);
}

static void sock_init(void)
{
}

static void sock_deinit(void)
{
}

static void sock_setnonblock(socket_t* s)
{
	if (s == NULL)
		return;
	fcntl(*s, F_SETFL, fcntl(*s, F_GETFL) | O_NONBLOCK);
}

static bool sock_wouldblock(void)
{
	return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS;
}

static void sock_close(socket_t* s)
{
	if (s == NULL)
		return;
	close(*s);
	*s = INVALID_SOCKET;
}

static void ctrl_c_handler(int signal)
{
	run = false;
}

static void ctrl_c_setup(void)
{
	struct sigaction handler = {0};
	sigemptyset(&handler.sa_mask);
	handler.sa_handler = ctrl_c_handler;
	handler.sa_flags = 0;

	sigaction(SIGINT, &handler, NULL);
}

#endif

static void sock_ensure(socket_t* s, const char* ip, int port)
{
	if (s == NULL)
		return;

	if (*s != INVALID_SOCKET)
		return;
//	{
//		char data;
//		if(recv(*s, &data, 1, MSG_PEEK) == SOCKET_ERROR)
//		{
//			if(sock_wouldblock())
//			{
//				printf("alive socket\n");
//				// alive socket
//				return;
//			}

//			printf("socket seems to be dead?\n");
//			sock_close(s);
//		}
//	}

	*s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if(*s == INVALID_SOCKET)
	{
		//printf("failed to create socket\n");
		//fflush(stdout);
		return;
	}

	sock_setnonblock(s);

	struct sockaddr_in addr;
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = inet_addr(ip);
	addr.sin_port = htons(port);

	//printf("trying to connect to %s:%i\n", ip, port);
	//fflush(stdout);

	if(connect(*s, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR)
	{
		if(!sock_wouldblock())
		{
			//printf("failed to connect socket\n");
			//fflush(stdout);

			sock_close(s);
			return;
		}
	}

	//printf("connected to %s:%i\n", ip, port);
	//fflush(stdout);

	char buffer[1024];
	int bytes = 0;
	int total_bytes = 0;
	while((bytes = recv(*s, buffer, sizeof(buffer), 0)) > 0)
		total_bytes += bytes;

	if (total_bytes > 0)
	{
		//printf("flushed %i stale bytes in recv socket\n", total_bytes);
		//fflush(stdout);
	}
}

int main()
{
	ctrl_c_setup();
	sock_init();

	socket_t knot1 = INVALID_SOCKET;
	socket_t knot2 = INVALID_SOCKET;

	short transaction_id = 0;

	while(run)
	{
		wait_a_bit(1000);

		// ------------------------

		bool wait_for_connection = (knot1 == INVALID_SOCKET || knot2 == INVALID_SOCKET);

		sock_ensure(&knot1, "192.168.1.36", 502);
		sock_ensure(&knot2, "192.168.1.37", 502);

		if (knot1 == INVALID_SOCKET || knot2 == INVALID_SOCKET)
		{
			wait_a_bit(1000);
			continue;
		}

		// ------------------------

		if (wait_for_connection)
			wait_a_bit(500); // wait a bit for connections to establish

		// ------------------------

		transaction_id++;
		if (transaction_id == 0)
			transaction_id++;

		// modbus tcp request
		uint8_t request[12] =
		{
			(transaction_id >> 8) & 0xff, transaction_id & 0xff, // transaction id
			0x00, 0x00, // type
			0x00, 0x06, // length
			0x0a, // slave-addr = 10
			0x04, // function = 4
			0x00, 0x00, // start register = 0
			0x00, 0x14, // register count = 20
		};

		if(send(knot2, (const char*)request, sizeof(request), 0) != sizeof(request))
		{
			//printf("failed to send modbus request to knot2\n");
			//fflush(stdout);
			sock_close(&knot2);
			continue;
		}

		// ------------------------

		uint8_t response[49];
		bool response_ok = false;
		bool timeout_response = false;

		for(int i = 0, total_length = 0; i < 10; ++i)
		{
			int length = recv(knot2, (char*)response + total_length, sizeof(response) - total_length, 0);
			if (length == -1 || length == 0)
			{
				fd_set select_set = {0};
				FD_ZERO(&select_set);
				FD_SET(knot2, &select_set);
				struct timeval select_time = {.tv_sec = 0, .tv_usec = 500000};
				select(0, &select_set, NULL, NULL, &select_time);
				continue;
			}

			//printf("recv %i %i\n", transaction_id, length);
			//fflush(stdout);

			total_length += length;
			if (total_length == 9 && response[7] == 0x84)
			{
				//printf("knot2 timedout\n");
				//fflush(stdout);

				response_ok = true;
				timeout_response = true;
				break;
			}

			if (total_length < (int)sizeof(response))
				continue;

			response_ok = true;
			break;
		}

		if (!response_ok)
		{
			//printf("failed to recv modbus response from knot2\n");
			//fflush(stdout);
			sock_close(&knot2);
			continue;
		}

		// ------------------------

		if(timeout_response == false && send(knot1, (const char*)response, sizeof(response), 0) != sizeof(response))
		{
			//printf("failed to send modbus response to knot1\n");
			//fflush(stdout);
			sock_close(&knot1);
			continue;
		}
	}

	sock_deinit();

	return 0;
}

