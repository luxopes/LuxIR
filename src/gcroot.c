#include "all.h"

/* Typed Value locals use a precise shadow-root frame. Insert before SSA so
 * stores follow every source-level definition, including Phi and reassigns.
 * A no-call leaf cannot collect under the single-owner ABI and needs no frame.
 */
static void
gcins(Ins **v, uint *n, int op, int cls, Ref to, Ref a, Ref b)
{
	Ins i;
	i.op = op; i.cls = cls; i.to = to; i.arg[0] = a; i.arg[1] = b;
	addins(v, n, &i);
}

static Ref
gcsymbol(char *name, Fn *fn)
{
	Con c;
	memset(&c, 0, sizeof c);
	c.type = CAddr; c.sym.type = SExt; c.sym.id = intern(name);
	return newcon(&c, fn);
}

static void
gcpop(Ins **v, uint *n, Ref heap, Ref frame, Ref status, Fn *fn)
{
	gcins(v, n, Oarg, Kl, R, heap, R);
	gcins(v, n, Oarg, Kl, R, frame, R);
	gcins(v, n, Ocall, Kw, status, gcsymbol("luxrt_roots_pop", fn), R);
}

void
gcprepare(Fn *fn)
{
	int t, ntmp, count;
	uint k, n, pars;
	Ref *slots, heap, frame, base, status, initial;
	Blk *b, *ret, *abort, *extra, **tail, **last;
	Phi *p;
	Ins *v, *i;

	if (req(fn->gcheap, R) || fn->leaf)
		return;
	ntmp = fn->ntmp;
	count = 0;
	for (t=Tmp0; t<ntmp; t++)
		count += fn->tmp[t].gcroot != 0;
	if (!count)
		return;
	slots = alloc(ntmp * sizeof slots[0]);
	heap = newtmp("gc.heap", Kl, fn);
	frame = newtmp("gc.frame", Kl, fn);
	base = newtmp("gc.slots", Kl, fn);
	abort = newblk(); abort->name = "gc.root.error"; abort->jmp.type = Jhlt;
	abort->id = fn->nblk++;
	extra = 0; tail = &extra;
	for (b=fn->start; b; b=b->link) {
		v = vnew(0, sizeof *v, PFn); n = 0; pars = 0;
		if (b == fn->start) {
			while (pars < b->nins && ispar(b->ins[pars].op)) {
				addins(&v, &n, &b->ins[pars]);
				pars++;
			}
			/* The original heap parameter may be reassigned in the body. */
			gcins(&v, &n, Ocopy, Kl, heap, fn->gcheap, R);
			gcins(&v, &n, Oalloc8, Kl, frame, getcon(24, fn), R);
			gcins(&v, &n, Oalloc8, Kl, base, getcon(count*8, fn), R);
			count = 0;
			for (t=Tmp0; t<ntmp; t++) {
				if (!fn->tmp[t].gcroot) continue;
				slots[t] = newtmp("gc.slot", Kl, fn);
				gcins(&v, &n, Oadd, Kl, slots[t], base, getcon(count++*8, fn));
				initial = getcon(0, fn);
				for (k=0; k<pars; k++)
					if (req(b->ins[k].to, TMP(t))) initial = TMP(t);
				gcins(&v, &n, Ostorel, Kw, R, initial, slots[t]);
			}
			gcins(&v, &n, Oarg, Kl, R, heap, R);
			gcins(&v, &n, Oarg, Kl, R, frame, R);
			gcins(&v, &n, Oarg, Kl, R, base, R);
			gcins(&v, &n, Oarg, Kl, R, getcon(count, fn), R);
			/* Fresh frame, valid context/span: push cannot fail by contract. */
			gcins(&v, &n, Ocall, Kw, R, gcsymbol("luxrt_roots_push", fn), R);
		}
		for (p=b->phi; p; p=p->link)
			if (rtype(p->to) == RTmp && p->to.val < (uint)ntmp && fn->tmp[p->to.val].gcroot)
				gcins(&v, &n, Ostorel, Kw, R, p->to, slots[p->to.val]);
		for (k=pars; k<b->nins; k++) {
			i = &b->ins[k]; addins(&v, &n, i);
			if (rtype(i->to) == RTmp && i->to.val < (uint)ntmp && fn->tmp[i->to.val].gcroot)
				gcins(&v, &n, Ostorel, Kw, R, i->to, slots[i->to.val]);
		}
		if (isret(b->jmp.type)) {
			ret = newblk(); ret->name = "gc.return"; ret->jmp = b->jmp;
			ret->id = fn->nblk++;
			*tail = ret; tail = &ret->link;
			status = newtmp("gc.pop.status", Kw, fn);
			gcpop(&v, &n, heap, frame, status, fn);
			b->jmp.type = Jjnz; b->jmp.arg = status;
			b->s1 = abort; b->s2 = ret;
		}
		b->ins = v; b->nins = n;
	}
	*tail = abort;
	last = &fn->start;
	while (*last) last = &(*last)->link;
	*last = extra;
	vgrow(&fn->rpo, fn->nblk);
	fn->leaf = 0;
}
