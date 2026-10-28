/* Count how often TLMM GPIO inputs read 1: pinsample <gpio>... */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	int fd = open("/dev/mem", O_RDONLY | O_SYNC);
	if (fd < 0) { perror("/dev/mem"); return 1; }
	for (int a = 1; a < argc; a++) {
		int g = atoi(argv[a]);
		off_t base = 0x01010000 + 0x1000 * g;
		volatile uint32_t *r = mmap(NULL, 0x1000, PROT_READ, MAP_SHARED, fd, base);
		if (r == MAP_FAILED) { perror("mmap"); return 1; }
		long ones = 0, n = 200000, edges = 0;
		uint32_t prev = r[1] & 1;
		for (long i = 0; i < n; i++) {
			uint32_t v = r[1] & 1;
			ones += v;
			edges += v != prev;
			prev = v;
		}
		printf("gpio%d: ones %ld of %ld (%.1f%%), changes %ld\n", g, ones, n, 100.0 * ones / n, edges);
		munmap((void *)r, 0x1000);
	}
	return 0;
}
