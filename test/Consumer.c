#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h> 
#include <sys/stat.h> 
#include <fcntl.h>
#include <string.h>
#include <stdio.h>

int main(int argc, char* argv[]) {
	int nitems = atoi(argv[1]);
	int i, fd, n;
	char cons_info[32];
	fcntl(0, F_SETFL, fcntl(0,F_GETFL) | O_NONBLOCK); /* stdin */
	fcntl(1, F_SETFL, fcntl(1,F_GETFL) | O_NONBLOCK); /* stdout */
	fd = open("/dev/scullpipe0", O_RDONLY);
	fprintf(stderr, "Consumer going to sleep\n");
	sleep(1);
	fprintf(stderr, "Consumer being wake up!\n");
	for(i = 0; i < nitems; i++) {
		n = read(fd, cons_info, 12);
		sleep(1);
		if(n == -1) {
			fprintf(stderr, "Consumer reading failed!\n");
			close(fd);
			return -1;
		}
		if(n == 0) {
			fprintf(stderr, "Consumer reading nothing!\n");
			close(fd);
			return 0;
		}
		else
			fprintf(stderr, "Consumer read: %s\n", cons_info);
	}
	close(fd);
	return 0;
}
