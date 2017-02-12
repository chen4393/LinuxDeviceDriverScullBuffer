#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h> 
#include <sys/stat.h> 
#include <fcntl.h>
#include <string.h>
#include <stdio.h>

int main(int argc, char* argv[]) {
	int nitems = atoi(argv[2]);
	int i, fd, n;
	int delay = 1;
	fcntl(0, F_SETFL, fcntl(0,F_GETFL) | O_NONBLOCK); /* stdin */
	fcntl(1, F_SETFL, fcntl(1,F_GETFL) | O_NONBLOCK); /* stdout */
	char prod_info[32];
	fd = open("/dev/scullpipe0", O_WRONLY);
	fprintf(stderr, "Producer %s going to sleep\n", argv[1]);
	sleep(delay);
	fprintf(stderr, "Producer %s being wake up!\n", argv[1]);
	for(i = 0; i < nitems; i++) {
		sprintf(prod_info, "%s%06d", argv[1], i+1);
		n = write(fd, prod_info, strlen(prod_info)+1);
		sleep(1);
		if(n == -1) {
			fprintf(stderr, "Producer %s writing failed!\n", argv[1]);
			close(fd);
			return -1;
		}
		if(n == 0) {
			fprintf(stderr, "Producer %s writing nothing!\n", argv[1]);
			close(fd);
			return 0;
		}
		fprintf(stderr, "Producer wrote: %s\n", prod_info);
	}
	close(fd);
	return 0;
}
