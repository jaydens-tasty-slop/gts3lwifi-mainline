/* mmio-wr <phys-addr> <value>: write a 32-bit MMIO word via /dev/mem, print before/after */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/mman.h>
int main(int argc, char **argv)
{
	unsigned long a = strtoul(argv[1], 0, 16), pg = a & ~0xfffUL;
	uint32_t v = strtoul(argv[2], 0, 16);
	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) { perror("open"); return 1; }
	volatile uint32_t *m = mmap(0, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, pg);
	if (m == MAP_FAILED) { perror("mmap"); return 1; }
	volatile uint32_t *p = m + (a - pg) / 4;
	uint32_t old = *p;
	*p = v;
	printf("%08lx: %08x -> %08x\n", a, old, *p);
	return 0;
}
