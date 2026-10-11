/* Independent per-bit count, covering empty/dense/sparse/high-bit words. */
#include "../src/all.h"

int
main(void)
{
	BSet bs = {0};
	bits word, state = 0x8c9ab17e52f136d1ULL;
	uint count, mode, round, i, bit, expected;
	unsigned long checks = 0;

	bs.t = calloc(257, sizeof *bs.t);
	assert(bs.t);
	for (count=0; count<=257; count++)
	for (mode=0; mode<6; mode++)
	for (round=0; round<8; round++) {
		bs.nt = count;
		expected = 0;
		for (i=0; i<count; i++) {
			state ^= state << 13;
			state ^= state >> 7;
			state ^= state << 17;
			switch (mode) {
			case 0: word = 0; break;
			case 1: word = ~(bits)0; break;
			case 2: word = (bits)1 << (i % 64); break;
			case 3: word = state; break;
			case 4: word = i % 17 ? 0 : state; break;
			default: word = (bits)1 << 63; break;
			}
			bs.t[i] = word;
			for (bit=0; bit<64; bit++)
				expected += (word >> bit) & 1;
		}
		assert(bscount(&bs) == expected);
		checks++;
	}
	free(bs.t);
	printf("bitset count: %lu independent checks PASS\n", checks);
	return 0;
}
