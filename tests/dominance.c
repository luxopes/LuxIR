/* Independent dominance oracle: remove a vertex and test reachability.
 * Build without other backend objects; unused cfg sections are discarded:
 * cc -O2 -std=c99 -ffunction-sections -fdata-sections -Wl,--gc-sections \
 *    tests/dominance.c src/cfg.c -o build/dominance-test
 */
#include "../src/all.h"

static unsigned random_state = 1;

/* The standalone cfg test only needs zeroed scratch allocation. */
void *
emalloc(size_t n)
{
	void *p = calloc(1, n);
	assert(p);
	return p;
}

static unsigned
random_word(void)
{

	random_state ^= random_state << 13;
	random_state ^= random_state >> 17;
	random_state ^= random_state << 5;
	return random_state;
}

static void
postorder(Blk *b, unsigned char *seen, Blk **order, unsigned *count)
{
	unsigned index = b->visit;

	if (seen[index])
		return;
	seen[index] = 1;
	if (b->s1)
		postorder(b->s1, seen, order, count);
	if (b->s2)
		postorder(b->s2, seen, order, count);
	order[(*count)++] = b;
}

static int
oracle(Blk *blocks, unsigned n, unsigned removed, unsigned target)
{
	unsigned *queue, head, tail, index, k;
	unsigned char *seen;
	Blk *successors[2];
	int result;

	if (removed == target)
		return 0; /* strict dominance */
	if (removed == 0)
		return 1;
	queue = calloc(n, sizeof *queue);
	seen = calloc(n, 1);
	assert(queue && seen);
	head = 0;
	tail = 1;
	queue[0] = 0;
	seen[0] = 1;
	while (head < tail) {
		index = queue[head++];
		successors[0] = blocks[index].s1;
		successors[1] = blocks[index].s2;
		for (k=0; k<2; k++) {
			if (!successors[k])
				continue;
			index = successors[k]->visit;
			if (index != removed && !seen[index]) {
				seen[index] = 1;
				queue[tail++] = index;
			}
		}
	}
	result = !seen[target];
	free(seen);
	free(queue);
	return result;
}

static unsigned long
check_graph(unsigned n, unsigned seed, int chain)
{
	Fn fn = {0};
	Blk *blocks, **order, *b;
	unsigned char *seen;
	unsigned i, j, k, count;
	unsigned long checks = 0;

	blocks = calloc(n, sizeof *blocks);
	order = calloc(n, sizeof *order);
	seen = calloc(n, 1);
	assert(blocks && order && seen);
	random_state = seed;
	for (i=0; i<n; i++) {
		b = &blocks[i];
		b->visit = i;
		b->pred = calloc(n*2, sizeof *b->pred);
		assert(b->pred);
		if (i+1<n) {
			b->s1 = &blocks[i+1];
			b->link = &blocks[i+1];
		}
		if (!chain && (random_word() & 3) != 0)
			b->s2 = &blocks[random_word() % n];
	}
	for (i=0; i<n; i++) {
		b = &blocks[i];
		if (b->s1)
			b->s1->pred[b->s1->npred++] = b;
		if (b->s2 && b->s2 != b->s1)
			b->s2->pred[b->s2->npred++] = b;
	}
	count = 0;
	postorder(blocks, seen, order, &count);
	assert(count == n);
	fn.rpo = calloc(n, sizeof *fn.rpo);
	assert(fn.rpo);
	for (i=0; i<n; i++) {
		fn.rpo[i] = order[n-i-1];
		fn.rpo[i]->id = i;
	}
	fn.start = blocks;
	fn.nblk = n;
	/* Repeated rebuilding also detects stale intervals/tree traversal links. */
	for (k=0; k<2; k++) {
		filldom(&fn);
		for (i=0; i<n; i++)
			for (j=0; j<n; j++) {
				if (chain && n>256 && i!=0 && i!=n/2 && i!=n-1)
					continue;
				assert(sdom(&blocks[i], &blocks[j]) == oracle(blocks, n, i, j));
				checks++;
			}
	}
	for (i=0; i<n; i++)
		free(blocks[i].pred);
	free(fn.rpo);
	free(seen);
	free(order);
	free(blocks);
	return checks;
}

int
main(void)
{
	unsigned seed, n;
	unsigned long checks = 0;

	for (n=1; n<=65; n+=4)
		for (seed=1; seed<=64; seed++)
			checks += check_graph(n, seed, 0);
	checks += check_graph(2048, 1, 1);
	printf("dominance: %lu independent reachability checks PASS\n", checks);
	return 0;
}
