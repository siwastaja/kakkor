#ifndef __COM_UART_H
#define __COM_UART_H


int open_device(char* device);
int close_device(int fd);

// Reads until finds two message separator characters (';'),
// or timeouts. Copies the data between the separators to
// outbuf.
int read_reply(int fd, char* outbuf, int maxbytes);

// Calls read_reply to internal buffer, compares it with buf,
// returns 0 if match. Great for checking fixed OK messages.
int comm_expect(int fd, char* buf);

int comm_send(int fd, char* buf);
void uart_flush(int fd);




#endif
