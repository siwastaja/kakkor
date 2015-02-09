#include <inttypes.h>
#include <termios.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdio_ext.h>
#include <stdlib.h>
#include <fcntl.h>
#include <stropts.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <errno.h>

int cur_fd = 5;

int open_device(char* device)
{
	printf("        UART_SIMU: Opened device %s, gave fd = %d\n", device, cur_fd++);
	return 0;
}

int close_device(int fd)
{
	printf("        UART_SIMU: Closed defice fd = %d\n", fd);
	return 0;
}

void uart_flush(int fd)
{
	return;
}

int comm_send(int device_fd, char* buf)
{
	printf("        UART_SIMU: tx to fd=%d: %s\n", device_fd, buf);
	return 0;
}


#define COMM_SEPARATOR ';'
#define MAX_READBUF_LEN 500

#define REPLY_WAIT_TIMEOUT_MS 200
#define REPLY_INTERREAD_TIMEOUT_MS 20

int read_reply(int fd, char* outbuf, int maxbytes)
{
	printf("        UART_SIMU: Give reply (fd=%d) > ", fd); fflush(stdout);
	gets(outbuf);
	return 0;
}

int comm_expect(int fd, char* buf)
{
	char readbuf[1000];
	int ret;
	if((ret = read_reply(fd, readbuf, 1000)))
	{
		return ret;
	}
	if(strncmp(readbuf, buf, 1000) == 0)
		return 0;
	else
		return -999;
}

// Sends sendbuf, expects expect, sets the result AFTER expect buffer to rxbuf, returns 0
// In case of error, autoretries, and returns negative if failure.
int comm_autoretry(int fd, char* sendbuf, char* expect, char* rxbuf)
{
	char readbuf[1000];
	int retry = 0;
	while(1)
	{
		int ret;
		comm_send(fd, sendbuf);
		if((ret = read_reply(fd, readbuf, 1000)) == 0)
		{
			int len = strlen(expect);
			if(strncmp(readbuf, expect, len) == 0)
			{
				if(rxbuf)
					strcpy(rxbuf, readbuf+len);
				return 0;
			}
			else
			{
				printf("comm_autoretry: reply (%s) not as expected (%s) -- ", readbuf, expect);
			}
		}
		else
		{
			printf("comm_autoretry: read_reply returned %d -- ", ret);
		}
		retry++;
		uart_flush(fd);
		if(retry > 5)
		{
			printf("out of autoretries, giving up.\n"); 
			return -1;
		}
		int sleepy = retry*retry*retry;
		printf("autoretry #%d after sleeping %d ms...\n", retry, sleepy);
		usleep(1000*sleepy);
	}
	return -1;
}

