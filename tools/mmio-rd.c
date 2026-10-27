/* mmio-rd <phys-addr> [count]: read 32-bit MMIO words via /dev/mem */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>
int main(int argc, char **argv)
{
	unsigned long a = strtoul(argv[1], 0, 16), n = argc > 2 ? strtoul(argv[2], 0, 0) : 1;
	unsigned long pg = a & ~0xfffUL;
	int fd = open("/dev/mem", O_RDONLY | O_SYNC);
	if (fd < 0) { perror("open"); return 1; }
	volatile uint32_t *m = mmap(0, 0x2000, PROT_READ, MAP_SHARED, fd, pg);
	if (m == MAP_FAILED) { perror("mmap"); return 1; }
	for (unsigned long i = 0; i < n; i++)
		printf("%08lx: %08x\n", a + 4 * i, m[(a - pg) / 4 + i]);
	return 0;
}
