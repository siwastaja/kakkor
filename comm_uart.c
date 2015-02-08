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

int set_interface_attribs(int fd)
{
	struct termios tty;
	memset(&tty, 0, sizeof(tty));
	if(tcgetattr(fd, &tty) != 0)
	{
		printf("error %d from tcgetattr\n", errno);
		return -1;
	}

	cfsetospeed(&tty, B115200);
	cfsetispeed(&tty, B115200);

	tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;
	tty.c_iflag &= ~IGNBRK;
	tty.c_iflag &= ~INLCR;
	tty.c_iflag &= ~ICRNL;
	tty.c_lflag = 0;
	tty.c_oflag = 0;
	tty.c_oflag &= ~ONLCR;
	tty.c_oflag &= ~OCRNL;
	tty.c_oflag &= ~ONLRET;
	tty.c_oflag &= ~ONOCR;
	tty.c_cc[VMIN] = 0;
	tty.c_cc[VTIME] = 0;

	tty.c_iflag &= ~(IXON | IXOFF | IXANY);
	tty.c_cflag |= (CLOCAL | CREAD);
	tty.c_cflag &= ~(PARENB | PARODD);
	tty.c_cflag &= ~CSTOPB;
//	tty.c_cflag |= CSTOPB;
	tty.c_cflag &= ~CRTSCTS;

	cfmakeraw(&tty);

	if(tcsetattr(fd, TCSANOW, &tty) != 0)
	{
		printf("error %d from tcsetattr\n", errno);
		return -1;
	}
	return 0;
}

int open_device(char* device)
{
	int fd;
	fd = open(device, O_RDWR | O_NOCTTY | O_SYNC | O_NONBLOCK);
	if(fd < 0)
	{
		printf("error %d opening %s: %s\n", errno, device, strerror(errno));
		return fd;
	}
	if(set_interface_attribs(fd))
		return  -1;

	return fd;
}

int close_device(int fd)
{
	return close(fd);
}

void uart_flush(int fd)
{
	tcflush(fd, TCIOFLUSH);
}

int comm_send(int devide_fd, char* buf)
{
	int len = 0;
	len = strlen(buf);
	if(len < 1) return -1;
	write(devide_fd, buf, len);
	return len;
}

/*
int aread(int fd, char* buf, int n)
{
	static int jutska = 0;
	switch(jutska)
	{
		case 0:
		strcpy(buf, "kakkenpis sen vir\ntsen:629"); break;
		case 1:
		strcpy(buf, "johannes;@1:VMEAS=123 IMEAS=45"); break;
		case 2:
		strcpy(buf, "6 TMEAS=789"); break;
		case 3:
		strcpy(buf, ";asfgdashjughaeghu;gta"); break;
		case 4:
		strcpy(buf, "safsa;@MUOVIKUKKA;;@kakkapissa"); break;
		case 5:
		strcpy(buf, ";;@kakka;;"); break;
		case 6:
		strcpy(buf, "@viela yksi"); break;
		case 7:
		strcpy(buf, ";"); break;
		default:
		buf[0] = 0;
		break;
	}

	printf("aread will return: %s\n", buf);
	jutska++;
	return strlen(buf);
}
*/


#define COMM_SEPARATOR ';'
#define MAX_READBUF_LEN 500

#define REPLY_WAIT_TIMEOUT_MS 200
#define REPLY_INTERREAD_TIMEOUT_MS 20

int read_reply(int fd, char* outbuf, int maxbytes)
{
	char readbuf[MAX_READBUF_LEN];
	char* p_readbuf = readbuf;
	char* p_start = NULL;
	char* p_end;
	char* p_readtime_readbuf;
	int timeout_cnt = 0;
	int firstwait_timeout_cnt = 0;
	int got_something = 0;

	if(maxbytes > MAX_READBUF_LEN-1)
		maxbytes = MAX_READBUF_LEN-1;

//	printf("read_reply"); fflush(stdout);
	while(1)
	{
		int bytes_read;
		p_readtime_readbuf = p_readbuf;
		bytes_read = read(fd, p_readbuf, maxbytes);
//		printf("."); fflush(stdout);
		if(bytes_read > 0)
		{
			got_something = 1;
			p_readbuf[bytes_read] = 0;
			maxbytes -= bytes_read;
			while((p_readbuf = strchr(p_readbuf, COMM_SEPARATOR)))
			{
				if(p_start == NULL)
				{
					p_start = ++p_readbuf;
				}
				else
				{
					p_end = p_readbuf;

					if(p_start >= p_end)
						return -1;

					*p_end = 0;
					strcpy(outbuf, p_start);
					return 0;
				}
			}
			if(maxbytes < 1)
				return -2;
			p_readbuf = p_readtime_readbuf + bytes_read;
		}
		if(got_something)
			timeout_cnt++;
		firstwait_timeout_cnt++;

		if(!got_something && firstwait_timeout_cnt > REPLY_WAIT_TIMEOUT_MS)
			return -3;
		if(got_something && timeout_cnt > REPLY_INTERREAD_TIMEOUT_MS)
			return -4;

		usleep(1000);
	}
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
		printf("autoretry #%d after sleeping %d ms...\n", sleepy);
		usleep(1000*sleepy);
	}
	return -1;
}

