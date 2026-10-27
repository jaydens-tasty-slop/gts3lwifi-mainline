/*
 * spmi-reg <sid> <addr> [value]: read (or write) one PMIC register through the
 * MSM8996 SPMI PMIC arbiter v2 via /dev/mem, using the APPS (EE 0) channel.
 * Mirrors drivers/spmi/spmi-pmic-arb.c: APID map at core+0x800, owner at
 * cnfg+0x700, channel at base + 0x8000*apid, CMD 0x0, STATUS 0x8,
 * WDATA0 0x10, RDATA0 0x18, cmd = opc<<27 | (addr&0xff)<<4 | bc.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>

static int fd;
static volatile uint32_t *map(unsigned long pa, unsigned long len)
{
	void *p = mmap(0, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, pa);
	if (p == MAP_FAILED) { perror("mmap"); exit(1); }
	return p;
}

static int wait_done(volatile uint32_t *ch)
{
	for (int i = 0; i < 10000; i++) {
		uint32_t s = ch[0x8 / 4];
		if (s & 1)
			return (s & 0xe) ? -(int)s : 0;
		usleep(1);
	}
	return -1000;
}

int main(int argc, char **argv)
{
	unsigned sid = strtoul(argv[1], 0, 0), addr = strtoul(argv[2], 0, 16);
	unsigned ppid = sid << 8 | (addr >> 8 & 0xff), apid;
	fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) { perror("open"); return 1; }
	volatile uint32_t *core = map(0x0400f000, 0x1000), *cnfg = map(0x0400a000, 0x3000);
	for (apid = 0; 0x800 + 4 * apid < 0x1000; apid++)
		if (core[(0x800 + 4 * apid) / 4] && ((core[(0x800 + 4 * apid) / 4] >> 8) & 0xfff) == ppid)
			break;
	if (0x800 + 4 * apid >= 0x1000) { fprintf(stderr, "ppid %03x not mapped\n", ppid); return 1; }
	unsigned owner = cnfg[(0x700 + 4 * apid) / 4] & 7;
	volatile uint32_t *rd = map(0x04c00000 + 0x8000UL * apid, 0x1000);
	rd[0] = 1u << 27 | (addr & 0xff) << 4;
	int r = wait_done(rd);
	unsigned old = rd[0x18 / 4] & 0xff;
	if (r) { fprintf(stderr, "read failed %d (apid %u owner %u)\n", r, apid, owner); return 1; }
	if (argc < 4) { printf("%x.%04x = %02x (apid %u owner %u)\n", sid, addr, old, apid, owner); return 0; }
	if (owner != 0) { fprintf(stderr, "apid %u owned by EE %u, not writing\n", apid, owner); return 1; }
	volatile uint32_t *wr = map(0x04400000 + 0x8000UL * apid, 0x1000);
	wr[0x10 / 4] = strtoul(argv[3], 0, 16) & 0xff;
	wr[0] = 0u << 27 | (addr & 0xff) << 4;
	r = wait_done(wr);
	rd[0] = 1u << 27 | (addr & 0xff) << 4;
	wait_done(rd);
	printf("%x.%04x: %02x -> %02x%s\n", sid, addr, old, rd[0x18 / 4] & 0xff, r ? " (write status error)" : "");
	return r ? 1 : 0;
}
