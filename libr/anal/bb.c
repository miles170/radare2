/* radare - LGPL - Copyright 2010-2011 */
/*   nibble<.ds@gmail.com> */
/*   pancake<nopcode.org> */

#include <r_anal.h>
#include <r_util.h>
#include <r_list.h>

#define VERBOSE if(0)

R_API RAnalBlock *r_anal_bb_new() {
	RAnalBlock *bb = R_NEW (RAnalBlock);
	if (bb) {
		memset (bb, 0, sizeof (RAnalBlock));
		bb->addr = -1;
		bb->jump = -1;
		bb->fail = -1;
		bb->type = R_ANAL_BB_TYPE_NULL;
		bb->aops = r_anal_aop_list_new();
		bb->cond = NULL;
		bb->fingerprint = NULL;
		bb->diff = r_anal_diff_new ();
	}
	return bb;
}

R_API void r_anal_bb_trace(RAnal *anal, ut64 addr) {
	RAnalBlock *bbi;
	RListIter *iter;
	VERBOSE eprintf("bbtraced\n"); // XXX Debug msg
	r_list_foreach (anal->bbs, iter, bbi) {
		if (addr>=bbi->addr && addr<(bbi->addr+bbi->size)) {
			bbi->traced = R_TRUE;
			break;
		}
	}
}

R_API RList *r_anal_bb_list_new() {
	RList *list = r_list_new ();
	list->free = &r_anal_bb_free;
	return list;
}

R_API void r_anal_bb_free(void *_bb) {
	RAnalBlock *bb = _bb;
	if (bb) {
		if (bb->cond)
			free (bb->cond);
		if (bb->aops)
			r_list_free (bb->aops);
		if (bb->fingerprint)
			free (bb->fingerprint);
		if (bb->diff)
			r_anal_diff_free (bb->diff);
		free (bb);
	}
}

R_API int r_anal_bb(RAnal *anal, RAnalBlock *bb, ut64 addr, ut8 *buf, ut64 len, int head) {
	RAnalOp *aop = NULL;
	int oplen, idx = 0;

	if (bb->addr == -1)
		bb->addr = addr;
	while (idx < len) {
		if (!(aop = r_anal_aop_new())) {
			eprintf ("Error: new (aop)\n");
			return R_ANAL_RET_ERROR;
		}
		if ((oplen = r_anal_aop (anal, aop, addr+idx, buf+idx, len-idx)) == 0) {
			r_anal_aop_free (aop);
			if (idx == 0) {
				VERBOSE eprintf ("Unknown opcode at 0x%08"PFMT64x"\n", addr+idx);
				return R_ANAL_RET_END;
			} else break;
		}
		idx += oplen;
		bb->size += oplen;
		bb->ninstr++;
		r_list_append (bb->aops, aop);
		if (head) bb->type = R_ANAL_BB_TYPE_HEAD;
		switch (aop->type) {
		case R_ANAL_OP_TYPE_CALL:
		case R_ANAL_OP_TYPE_UCALL:
			bb->ncalls++;
			break;
		case R_ANAL_OP_TYPE_CMP:
			bb->cond = r_anal_cond_new_from_aop (aop);
			break;
		case R_ANAL_OP_TYPE_CJMP:
			if (bb->cond) {
				// TODO: get values from anal backend
				bb->cond->type = R_ANAL_COND_EQ;
			} else VERBOSE
				eprintf ("Unknown conditional for block 0x%"PFMT64x"\n", bb->addr);
			bb->conditional = 1;
			bb->fail = aop->fail;
			bb->jump = aop->jump;
			bb->type |= R_ANAL_BB_TYPE_BODY;
			return R_ANAL_RET_END;
		case R_ANAL_OP_TYPE_JMP:
			bb->jump = aop->jump;
			bb->type |= R_ANAL_BB_TYPE_BODY;
			return R_ANAL_RET_END;
		case R_ANAL_OP_TYPE_UJMP:
			bb->type |= R_ANAL_BB_TYPE_FOOT;
			return R_ANAL_RET_END;
		case R_ANAL_OP_TYPE_RET:
			bb->type |= R_ANAL_BB_TYPE_LAST;
			bb->ncalls++;
			return R_ANAL_RET_END;
		}
	}
	return bb->size;
}

R_API int r_anal_bb_split(RAnal *anal, RAnalBlock *bb, RList *bbs, ut64 addr) {
	RAnalBlock *bbi;
	RAnalOp *aopi;
	RListIter *iter;

	r_list_foreach (bbs, iter, bbi)
		if (addr == bbi->addr)
			return R_ANAL_RET_DUP;
		else if (addr > bbi->addr && addr < bbi->addr + bbi->size) {
			r_list_append (bbs, bb);
			bb->addr = addr;
			bb->size = bbi->addr + bbi->size - addr;
			bb->jump = bbi->jump;
			bb->fail = bbi->fail;
			bbi->size = addr - bbi->addr;
			bbi->jump = addr;
			bbi->fail = -1;
			if (bbi->type&R_ANAL_BB_TYPE_HEAD) {
				bb->type = bbi->type^R_ANAL_BB_TYPE_HEAD;
				bbi->type = R_ANAL_BB_TYPE_HEAD;
			} else {
				bb->type = bbi->type;
				bbi->type = R_ANAL_BB_TYPE_BODY;
			}
			iter = r_list_iterator (bbi->aops);
			while (r_list_iter_next (iter)) {
				aopi = r_list_iter_get (iter);
				if (aopi->addr >= addr) {
					r_list_split (bbi->aops, aopi);
					bbi->ninstr--;
					r_list_append (bb->aops, aopi);
					bb->ninstr++;
				}
			}
			return R_ANAL_RET_END;
		}
	return R_ANAL_RET_NEW;
}

R_API int r_anal_bb_overlap(RAnal *anal, RAnalBlock *bb, RList *bbs) {
	RAnalBlock *bbi;
	RAnalOp *aopi;
	RListIter *iter;

	r_list_foreach (bbs, iter, bbi)
		if (bb->addr+bb->size > bbi->addr && bb->addr+bb->size <= bbi->addr+bbi->size) {
			bb->size = bbi->addr - bb->addr;
			bb->jump = bbi->addr;
			bb->fail = -1;
			if (bbi->type&R_ANAL_BB_TYPE_HEAD) {
				bb->type = R_ANAL_BB_TYPE_HEAD;
				bbi->type = bbi->type^R_ANAL_BB_TYPE_HEAD;
			} else bb->type = R_ANAL_BB_TYPE_BODY;
			r_list_foreach (bb->aops, iter, aopi)
				if (aopi->addr >= bbi->addr)
					r_list_unlink (bb->aops, aopi);
			r_list_append (bbs, bb);
			return R_ANAL_RET_END;
		}
	return R_ANAL_RET_NEW;
}

R_API int r_anal_bb_add(RAnal *anal, ut64 addr, ut64 size, ut64 jump, ut64 fail, int type, RAnalDiff *diff) {
	RAnalBlock *bb = NULL, *bbi;
	RListIter *iter;
	int append = 0, mid = 0;

	r_list_foreach (anal->bbs, iter, bbi) {
		if (addr == bbi->addr) {
			bb = bbi;
			mid = 0;
			break;
		} else if (addr > bbi->addr && addr < bbi->addr+bbi->size)
			mid = 1;
	}
	if (mid)
		return R_FALSE;
	if (bb == NULL) {
		if (!(bb = r_anal_bb_new ()))
			return R_FALSE;
		append = 1;
	}
	bb->addr = addr;
	bb->size = size;
	bb->jump = jump;
	bb->fail = fail;
	bb->type = type;
	if (diff) {
		bb->diff->type = diff->type;
		bb->diff->addr = diff->addr;
		R_FREE (bb->diff->name);
		if (diff->name)
			bb->diff->name = strdup (diff->name);
	}
	if (append) r_list_append (anal->bbs, bb);
	return R_TRUE;
}

R_API int r_anal_bb_del(RAnal *anal, ut64 addr) {
	RAnalBlock *bbi;
	RListIter *iter;
	ut64 jump, fail;

	if (addr == 0) {
		r_list_free (anal->bbs);
		if (!(anal->bbs = r_anal_bb_list_new ()))
			return R_FALSE;
	} else {
		r_list_foreach (anal->bbs, iter, bbi) {
			if (addr >= bbi->addr && addr < bbi->addr+bbi->size) {
				jump = bbi->jump;
				fail = bbi->fail;
				r_list_unlink (anal->bbs, bbi);
				if (fail != -1)
					r_anal_bb_del (anal, fail);
				if (jump != -1)
					r_anal_bb_del (anal, jump);
			}
		}
	}
	return R_TRUE;
}
