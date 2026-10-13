#include "all.h"

/* Bounded streaming inliner.  -i promises local binding of definitions.
 * Cache detached IR, never pointers into the per-function allocation pool.
 * Only already-seen single-block scalar leaves are eligible. */
typedef struct Inl Inl;
struct Inl {
	uint32_t symbol;
	int np, ni, nc, retcls;
	int cls[14];
	Ins ins[8];
	Con con[16];
	Ref ret;
};
static Inl *cache[128];
static int ncache;

void
inlreset(void)
{
	int n;

	for (n=0; n<ncache; n++)
		free(cache[n]);
	ncache = 0;
}

static int
inlop(int op)
{
	return INRANGE(op, Oadd, Opopcount)
		|| INRANGE(op, Oceqw, Ocuod)
		|| INRANGE(op, Oloadsb, Oload)
		|| INRANGE(op, Oextsb, Ocast)
		|| op == Ocopy;
}

/* Normalize constants and already-defined temporaries into bounded slots. */
static int
inlref(Ref r, Ref *out, Inl *s, int *ids, int n, Fn *fn)
{
	int k;
	Con *c;

	if (req(r, R)) {
		*out = R;
		return 1;
	}
	if (rtype(r) == RTmp) {
		for (k=0; k<n; k++)
			if ((uint)ids[k] == r.val) {
				*out = TMP(Tmp0+k);
				return 1;
			}
		return 0;
	}
	if (rtype(r) != RCon || r.val == 0 || s->nc == 16)
		return 0;
	c = &fn->con[r.val];
	if (c->type != CBits && c->type != CAddr)
		return 0;
	s->con[s->nc] = *c;
	*out = CON(s->nc++);
	return 1;
}

void
inlrecord(Fn *fn)
{
	Inl s, *p;
	Blk *b;
	Ins *i, *j;
	int ids[14], n, k;

	b = fn->start;
	if (ncache == 128 || b->link || b->phi || fn->vararg
	|| fn->dynalloc || fn->retty >= 0
	|| !INRANGE(b->jmp.type, Jretw, Jretd))
		return;
	memset(&s, 0, sizeof s);
	s.symbol = intern(fn->name);
	s.retcls = b->jmp.type - Jretw;
	n = 0;
	for (k=0; k<(int)b->nins; k++) {
		i = &b->ins[k];
		if (i->op == Onop)
			continue;
		if (rtype(i->to) != RTmp || i->to.val < Tmp0 || n == 14)
			return;
		/* Cached bodies must be SSA, including unused formal parameters. */
		for (int t=0; t<n; t++)
			if ((uint)ids[t] == i->to.val)
				return;
		if (i->op == Opar) {
			if (s.ni || s.np == 6)
				return;
			s.np++;
		} else {
			if (!inlop(i->op) || s.ni == 8)
				return;
			j = &s.ins[s.ni++];
			*j = *i;
			j->to = TMP(Tmp0+n);
			if (!inlref(i->arg[0], &j->arg[0], &s, ids, n, fn)
			|| !inlref(i->arg[1], &j->arg[1], &s, ids, n, fn))
				return;
		}
		ids[n] = i->to.val;
		s.cls[n++] = i->cls;
	}
	if (!inlref(b->jmp.arg, &s.ret, &s, ids, n, fn))
		return;
	p = emalloc(sizeof *p);
	*p = s;
	cache[ncache++] = p;
}

static Ref
inlmap(Ref r, Inl *s, Ref *tmp, Fn *fn)
{
	if (req(r, R))
		return R;
	if (rtype(r) == RTmp)
		return tmp[r.val-Tmp0];
	return newcon(&s->con[r.val], fn);
}

void
inlapply(Fn *fn)
{
	Blk *b;
	Ins *v, *i, x;
	Inl *s;
	Con *c;
	Ref tmp[14];
	uint nv, k, a, t;
	int h, added, valid;

	added = 0;
	fn->leaf = 1;
	for (b=fn->start; b; b=b->link) {
		v = vnew(0, sizeof *v, PFn);
		nv = 0;
		for (k=0; k<b->nins; k++) {
			i = &b->ins[k];
			s = 0;
			if (i->op == Ocall && rtype(i->arg[0]) == RCon
			&& req(i->arg[1], R) && added < 1024) {
				c = &fn->con[i->arg[0].val];
				if (c->type == CAddr && c->bits.i == 0
				&& (c->sym.type == SGlo || c->sym.type == SExt))
					for (h=ncache-1; h>=0; h--)
						if (cache[h]->symbol == c->sym.id) {
							s = cache[h];
							break;
						}
			}
			a = nv;
			while (a && isarg(v[a-1].op))
				a--;
			valid = s && (int)(nv-a) == s->np && i->cls == (uint)s->retcls
				&& added+s->np+s->ni+1 <= 1024;
			if (valid)
				for (t=a; t<nv; t++)
					if (v[t].op != Oarg || v[t].cls != (uint)s->cls[t-a])
						valid = 0;
			if (!valid) {
				addins(&v, &nv, i);
				if (i->op == Ocall)
					fn->leaf = 0;
				continue;
			}
			/* Freeze actuals before writing the original call destination. */
			for (t=0; t<(uint)s->np; t++) {
				tmp[t] = newtmp("inl", s->cls[t], fn);
				v[a+t].op = Ocopy;
				v[a+t].to = tmp[t];
			}
			for (t=0; t<(uint)s->ni; t++) {
				x = s->ins[t];
				tmp[s->np+t] = newtmp("inl", x.cls, fn);
				x.to = tmp[s->np+t];
				x.arg[0] = inlmap(x.arg[0], s, tmp, fn);
				x.arg[1] = inlmap(x.arg[1], s, tmp, fn);
				addins(&v, &nv, &x);
			}
			if (!req(i->to, R)) {
				x = *i;
				x.op = Ocopy;
				x.arg[0] = inlmap(s->ret, s, tmp, fn);
				x.arg[1] = R;
				addins(&v, &nv, &x);
			}
			added += s->np+s->ni+1;
		}
		b->ins = v;
		b->nins = nv;
	}
}
