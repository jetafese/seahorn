#include "BvOpSem2MemRepr.hh"
#include "seahorn/Expr/ExprAddrRangeMap.hh"
#include "seahorn/Expr/ExprMemUtils.h"
#include "seahorn/Expr/ExprOpBinder.hh"
#include "seahorn/Expr/ExprOpBool.hh"
#include "seahorn/Expr/ExprRewriter.hh"
#include "seahorn/Expr/ExprVisitor.hh"
#include "seahorn/Support/Stats.hh"

namespace {
template <typename T, typename... Rest>
auto as_std_array(const T &t, const Rest &... rest) ->
    typename std::array<T, sizeof...(Rest) + 1> {
  return {t, rest...};
}
} // namespace

#define DEBUG_TYPE "opsem"

namespace seahorn {
namespace details {

/**
 * rewriter for store expr:
 * e = select(store(A idx val), ptr) -> { return ite( (idx == ptr), val,
 *rewrite(A) )} e is not store (const array or terminal register) -> { return
 *select(e, m_ptr) }
 **/
struct ArrayStoreRewriter : public std::unary_function<Expr, Expr> {
  Expr m_ptr;
  OpSemAlu &m_alu;
  unsigned m_ptrSz;
  ArrayStoreRewriter(Expr ptr, OpSemAlu &alu, unsigned ptrSz)
      : m_ptr(ptr), m_alu(alu), m_ptrSz(ptrSz) {}
  ArrayStoreRewriter(const ArrayStoreRewriter &other)
      : m_ptr(other.m_ptr), m_alu(other.m_alu), m_ptrSz(other.m_ptrSz) {}

  Expr doPtrEq(Expr p1, Expr p2) { return m_alu.doEq(p1, p2, m_ptrSz); }

  Expr operator()(Expr e) {
    if (isOp<STORE>(e)) {
      Expr arr = e->arg(0);
      Expr idx = e->arg(1);
      Expr val = e->arg(2);
      Expr cond = doPtrEq(m_ptr, idx);
      Expr res = boolop::lite(cond, val,
                              arr // has been rewritten further
      );
      return res;
    } else {
      return op::array::select(e, m_ptr);
    }
  }
};

// Non-recursive rewrite of expr e
template <typename RW>
Expr arrayStoreRewrite(RW &rewriter, Expr ptr, Expr mem,
                       DagVisitMemCache &cache) {
  Expr cur = mem;

  // build rewrite stack
  ExprVector worklist = {cur};
  while (isOp<STORE>(cur)) {
    worklist.push_back(cur->arg(0));
    cur = cur->arg(0);
  }

  // rewrite from top of stack
  Expr res;
  while (!worklist.empty()) {
    Expr top = worklist.back();
    worklist.pop_back();
    Expr rw; // rewritten expr of top

    // first try find in cache
    DagVisitMemCache::const_iterator cit = cache.find(&*top);
    if (cit != cache.end()) {
      ExprPair cached = cit->second;
      if (ptr == cached.first) {
        res = cached.second;
        continue;
      }
    }

    if (isOp<STORE>(top)) {
      // make new store expr with rewritten array argument
      llvm::SmallVector<Expr, 4> new_kids = {res};
      for (auto b = top->args_begin() + 1; b != top->args_end(); ++b) {
        new_kids.push_back(*b);
      }
      rw =
          top->getFactory().mkNary(top->op(), new_kids.begin(), new_kids.end());
    } else {
      rw = top->getFactory().mkNary(top->op(), top->args_begin(),
                                    top->args_end());
    }

    // rewrite into ITE
    rw = rewriter(rw);
    // save to cache
    cache[&*top] = ExprPair(ptr, rw);
    // save for next level
    res = rw;
  }

  return res;
}

OpSemMemRepr::MemValTy OpSemMemArrayReprBase::MemSet(PtrTy ptr, Expr _val,
                                                     unsigned len, MemValTy mem,
                                                     unsigned wordSzInBytes,
                                                     PtrSortTy ptrSort,
                                                     uint32_t align) {
  // MemSet operates at word level.
  // _val must fit within a byte
  // _val is converted to a byte.
  // byte is converted to a word
  // e.g. _val = 0x1, len = 0x1, wordSzInBytes = 0x4 => 0x00000001
  // e.g. _val = 0x1, len = 0x4, wordSzInBytes = 0x4 => 0x00000001
  Expr res;

  unsigned width;
  if (bv::isBvNum(_val, width) && width == 8) {
    assert(wordSzInBytes <= sizeof(unsigned long));
    int byte = bv::toMpz(_val).get_ui();
    unsigned long val = 0;
    memset(&val, byte, wordSzInBytes);

    res = mem.toExpr();
    for (unsigned i = 0; i < len; i += wordSzInBytes) {
      Expr idx = m_memManager.ptrAdd(ptr, i).toExpr();
      res = op::array::store(
          res, idx, bv::bvnum(val, wordSzInBytes * m_BitsPerByte, m_efac));
    }
    return MemValTy(res);
  }

  return MemValTy(res);
}

// len is in bytes
// _val must fit within a byte
OpSemMemRepr::MemValTy OpSemMemArrayReprBase::MemSet(PtrTy ptr, Expr _val,
                                                     Expr len, MemValTy mem,
                                                     unsigned wordSzInBytes,
                                                     PtrSortTy ptrSort,
                                                     uint32_t align) {
  Expr res;

  unsigned width;
  Expr bvVal;
  // extend _val to current word size
  if (bv::isBvNum(_val, width)) {
    assert(width == 8);
    assert(wordSzInBytes <= sizeof(unsigned long));
    int byte = bv::toMpz(_val).get_ui();
    unsigned long val = 0;
    memset(&val, byte, wordSzInBytes);
    bvVal = bv::bvnum(val, wordSzInBytes * m_BitsPerByte, m_efac);
  } else {
    bvVal = _val;
    for (unsigned i = 1; i < wordSzInBytes; ++i) {
      bvVal = m_ctx.alu().Concat({bvVal, 8}, {bvVal, 8 * i});
    }
  }

  // write into memory
  res = mem.toExpr();
  // XXX assume that bit-width(len) == ptrSizeInBits
  auto bitWidth = m_memManager.ptrSizeInBits();
  Expr upperBound = m_ctx.alu().doAdd(
      len, m_ctx.alu().si(-static_cast<signed>(wordSzInBytes), bitWidth),
      bitWidth);

  for (unsigned i = 0; i < m_memCpyUnrollCnt; i += wordSzInBytes) {
    Expr idx = m_memManager.ptrAdd(ptr, i).toExpr();
    auto cmp =
        m_ctx.alu().doUle(m_ctx.alu().ui(i, m_memManager.ptrSizeInBits()),
                          upperBound, m_memManager.ptrSizeInBits());
    Expr ite = boolop::lite(cmp, bvVal, op::array::select(mem.toExpr(), idx));
    res = op::array::store(res, idx, ite);
  }

  LOG("opsem.array", errs() << "memset: " << *res << "\n";);
  return MemValTy(res);
}

// TODO: This function is untested
OpSemMemRepr::MemValTy OpSemMemArrayReprBase::MemCpy(
    PtrTy dPtr, PtrTy sPtr, Expr len, MemValTy memTrsfrRead, MemValTy memRead,
    unsigned wordSzInBytes, PtrSortTy ptrSort, uint32_t align) {
  (void)ptrSort;

  Expr res = memRead.toExpr();
  Expr srcMem = memTrsfrRead.toExpr();
  if (wordSzInBytes == 1 || (wordSzInBytes == 4 && align % 4 == 0) ||
      (wordSzInBytes == 8 && align % 4 == 0) ||
      m_memManager.isIgnoreAlignment()) {
    // XXX assume that bit-width(len) == ptrSizeInBits
    auto bitWidth = m_memManager.ptrSizeInBits();
    Expr upperBound = m_ctx.alu().doAdd(
        len, m_ctx.alu().si(-static_cast<signed>(wordSzInBytes), bitWidth),
        bitWidth);
    for (unsigned i = 0; i < m_memCpyUnrollCnt; i += wordSzInBytes) {
      Expr dIdx = m_memManager.ptrAdd(dPtr, i).toExpr();
      Expr sIdx = m_memManager.ptrAdd(sPtr, i).toExpr();
      auto cmp =
          m_ctx.alu().doUle(m_ctx.alu().ui(i, m_memManager.ptrSizeInBits()),
                            upperBound, m_memManager.ptrSizeInBits());
      auto ite = boolop::lite(cmp, op::array::select(srcMem, sIdx),
                              op::array::select(memRead.toExpr(), dIdx));
      res = op::array::store(res, dIdx, ite);
    }
    LOG("opsem.array", INFO << "memcpy: " << *res << "\n";);

  } else {
    DOG(ERR << "Word size and pointer are not aligned and "
               "alignment is not ignored!");
    DOG(ERR << "Try --horn-bv2-lambdas=true or --horn-bv2-word-size=1");
    assert(false);
  }
  return MemValTy(res);
}

OpSemMemRepr::MemValTy
OpSemMemArrayReprBase::MemCpy(PtrTy dPtr, PtrTy sPtr, unsigned len,
                              MemValTy memTrsfrRead, MemValTy memRead,
                              unsigned wordSzInBytes, PtrSortTy ptrSort,
                              uint32_t align) {
  (void)ptrSort;

  Expr res;

  if (wordSzInBytes == 1 || (wordSzInBytes == 4 && align % 4 == 0) ||
      (wordSzInBytes == 8 && align %  4 == 0) ||
      m_memManager.isIgnoreAlignment()) {
    Expr srcMem = memTrsfrRead.toExpr();
    res = memRead.toExpr();
    for (unsigned i = 0; i < len; i += wordSzInBytes) {
      Expr dIdx = m_memManager.ptrAdd(dPtr, i).toExpr();
      Expr sIdx = m_memManager.ptrAdd(sPtr, i).toExpr();

      Expr val = op::array::select(srcMem, sIdx);
      res = op::array::store(res, dIdx, val);
    }
  } else {
    DOG(ERR << "Word size and pointer are not aligned and "
               "alignment is not ignored!"
            << "\n");
    assert(false);
  }
  return MemValTy(res);
}

OpSemMemRepr::MemValTy
OpSemMemArrayReprBase::MemFill(PtrTy dPtr, char *sPtr, unsigned len,
                               MemValTy mem, unsigned wordSzInBytes,
                               PtrSortTy ptrSort, uint32_t align) {
  Expr res = mem.toExpr();
  const unsigned sem_word_sz = wordSzInBytes;

  // 8 bytes because assumed largest supported sem_word_sz = 8
  assert(sizeof(unsigned long) >= sem_word_sz);

  for (unsigned i = 0; i < len; i += sem_word_sz) {
    Expr dIdx = m_memManager.ptrAdd(dPtr, i).toExpr();
    // copy bytes from buffer to word - word must accommodate largest
    // supported word size
    // 8 bytes because assumed largest supported sem_word_sz = 8
    unsigned long word = 0;
    std::memcpy(&word, sPtr + i, sem_word_sz);
    Expr val = bv::bvnum(word, wordSzInBytes * m_BitsPerByte, m_efac);
    res = op::array::store(res, dIdx, val);
  }
  return MemValTy(res);
}

Expr OpSemMemHybridRepr::loadAlignedWordFromMem(PtrTy ptr, MemValTy mem) {
  LOG("opsem-hybrid", INFO << "load inst: " << m_ctx.getCurrentInst() << "\n");
  LOG("opsem-hybrid", INFO << "Load ptr " << *ptr.toExpr() << "\n");
  LOG("opsem-hybrid", INFO << "From mem " << *mem.toExpr() << "\n");
  /** rewrite store into ITE **/
  Stats::resume("hybrid-mem-rewrite");
  ArrayStoreRewriter rw(ptr.toExpr(), m_ctx.alu(),
                        m_memManager.ptrSizeInBits());
  Expr rewritten = arrayStoreRewrite(rw, ptr.toExpr(), mem.toExpr(), m_cache);
  LOG("opsem-hybrid", INFO << "Rewritten: " << *rewritten << "\n");

  // push bvadd down in ptr expr
  AddrRangeMap ptrArm;
  DagVisitCache ptrCache;
  Expr ptrSimp = rewriteMemExprWithCache<PointerArithmeticConfig>(
      ptr.toExpr(), ptrArm, ptrCache);
  LOG("opsem-hybrid", INFO << "Simp ptr: " << *ptrSimp << "\nbuilding ARM...");

  /** simplify with custom ITE simplifier **/
  AddrRangeMap arm = expr::mem::addrRangeMapOf(ptrSimp);
  LOG("opsem-hybrid", { INFO << "built addr range map: \n" << arm; });

  Expr simp = rewriteHybridLoadMemExpr(rewritten, ptr.toExpr(), arm);
  Stats::stop("hybrid-mem-rewrite");
  LOG("opsem-hybrid", INFO << "hybrid simplified: " << *simp << "\n");
  return simp;
}

OpSemMemRepr::MemValTy
OpSemMemLambdaRepr::storeAlignedWordToMem(Expr val, PtrTy ptr,
                                          PtrSortTy ptrSort, MemValTy mem) {
  PtrTy b0 = PtrTy(bind::bvar(0, ptrSort.toExpr()));

  Expr fappl = op::bind::fapp(mem.toExpr(), b0.toExpr());
  Expr ite = boolop::lite(m_memManager.ptrEq(b0, ptr), val, fappl);

  Expr addr =
      bind::mkConst(mkTerm<std::string>("addr", m_efac), ptrSort.toExpr());
  Expr decl = bind::fname(addr);
  return MemValTy(mk<LAMBDA>(decl, ite));
}

// len is in bytes
OpSemMemRepr::MemValTy OpSemMemLambdaRepr::MemSet(PtrTy ptr, Expr _val,
                                                  unsigned len, MemValTy mem,
                                                  unsigned wordSzInBytes,
                                                  PtrSortTy ptrSort,
                                                  uint32_t align) {
  Expr res;
  Expr bvVal;
  unsigned width;
  // -- expected width of 8 bits
  if (m_ctx.alu().isNum(_val, width)) {
    assert(width == 8);
    assert(wordSzInBytes <= sizeof(unsigned long));
    int byte = bv::toMpz(_val).get_ui();
    unsigned long val = 0;
    memset(&val, byte, wordSzInBytes);
    bvVal = bv::bvnum(val, wordSzInBytes * m_BitsPerByte, m_efac);
  } else {
    bvVal = _val;
    for (unsigned i = 1; i < wordSzInBytes; ++i) {
      bvVal = m_ctx.alu().Concat({bvVal, 8}, {bvVal, 8 * i});
    }
  }

  assert(bvVal);

  res = mem.toExpr();

  PtrTy last = m_memManager.ptrAdd(ptr, len - wordSzInBytes);
  PtrTy b0 = PtrTy(bind::bvar(0, ptrSort.toExpr()));

  Expr cmp = m_memManager.ptrInRangeCheck(ptr, b0, last);
  Expr fappl = op::bind::fapp(res, b0.toExpr());
  Expr ite = boolop::lite(cmp, bvVal, fappl);

  Expr addr =
      bind::mkConst(mkTerm<std::string>("addr", m_efac), ptrSort.toExpr());
  Expr decl = bind::fname(addr);
  res = mk<LAMBDA>(decl, ite);
  LOG("opsem.lambda", errs() << "MemSet " << *res << "\n");

  return MemValTy(res);
}

OpSemMemRepr::MemValTy OpSemMemLambdaRepr::MemSet(PtrTy ptr, Expr _val,
                                                  Expr len, MemValTy mem,
                                                  unsigned wordSzInBytes,
                                                  PtrSortTy ptrSort,
                                                  uint32_t align) {
  Expr res;
  Expr val;

  DOG(if (wordSzInBytes != 1) WARN << "memset: untested word size: "
                                   << wordSzInBytes);

  unsigned width;
  if (bv::isBvNum(_val, width)) {
    assert(width == 8);
    assert(wordSzInBytes <= sizeof(unsigned long));
    int byte = bv::toMpz(_val).get_ui();
    unsigned long uval = 0;
    if (byte)
      memset(&uval, byte, wordSzInBytes);
    val = m_ctx.alu().num(mpz_class(uval), wordSzInBytes * 8);
  } else {
    val = _val;
    for (unsigned i = 1; i < wordSzInBytes; ++i) {
      val = m_ctx.alu().Concat({val, 8}, {val, 8 * i});
    }
  }
  assert(val);

  PtrTy last = m_memManager.ptrAdd(m_memManager.ptrAdd(ptr, len),
                                   -static_cast<signed>(wordSzInBytes));

  Expr bvVal = val;
  PtrTy b0 = PtrTy(bind::bvar(0, ptrSort.toExpr()));

  Expr cmp = m_memManager.ptrInRangeCheck(ptr, b0, last);
  Expr fappl = op::bind::fapp(mem.toExpr(), b0.toExpr());
  Expr ite = boolop::lite(cmp, bvVal, fappl);

  Expr addr =
      bind::mkConst(mkTerm<std::string>("addr", m_efac), ptrSort.toExpr());
  Expr decl = bind::fname(addr);
  res = mk<LAMBDA>(decl, ite);
  LOG("opsem.lambda", errs() << "MemSet " << *res << "\n");

  return MemValTy(res);
}

OpSemMemRepr::MemValTy OpSemMemLambdaRepr::MemCpy(
    PtrTy dPtr, PtrTy sPtr, Expr len, MemValTy memTrsfrRead, MemValTy memRead,
    unsigned wordSzInBytes, PtrSortTy ptrSort, uint32_t align) {
  MemValTy srcMem = memTrsfrRead;
  // address of the last word that is copied into dst
  PtrTy dstLast = m_memManager.ptrAdd(m_memManager.ptrAdd(dPtr, len),
                                      -static_cast<signed>(wordSzInBytes));
  return createMemCpyExpr(dPtr, sPtr, memRead, ptrSort, srcMem, dstLast,
                          wordSzInBytes, align);
}

// TODO: Call this from concrete LambdaRepr::MemCpy also to
// remove duplicate code
OpSemMemRepr::MemValTy OpSemMemLambdaRepr::createMemCpyExpr(
    const PtrTy &dPtr, const PtrTy &sPtr, const MemValTy &memRead,
    const PtrSortTy &ptrSort, const MemValTy &srcMem, const PtrTy &dstLast,
    unsigned wordSzInBytes, uint32_t align) const {
  MemValTy res = MemValTy(Expr());
  if (wordSzInBytes == 1 || (wordSzInBytes == 4 && align % 4 == 0) ||
      (wordSzInBytes == 8 && align % 4 == 0) ||
      m_memManager.isIgnoreAlignment()) {
    PtrTy b0 = PtrTy(bind::bvar(0, ptrSort.toExpr()));
    // -- dPtr <= b0 <= dstLast
    Expr cmp = this->m_memManager.ptrInRangeCheck(dPtr, b0, dstLast);
    // -- offset == dPtr - sPtr
    Expr offset = this->m_memManager.ptrOffsetFromBase(dPtr, sPtr);
    // -- maps ptr in dst to ptr in src
    Expr readPtrInSrc = this->m_memManager.ptrAdd(b0, offset).toExpr();

    Expr readFromSrc = bind::fapp(srcMem.toExpr(), readPtrInSrc);
    Expr readFromDst = bind::fapp(memRead.toExpr(), b0.toExpr());

    Expr ite = boolop::lite(cmp, readFromSrc, readFromDst);
    Expr addr = bind::mkConst(mkTerm<std::string>("addr", this->m_efac),
                              ptrSort.toExpr());
    Expr decl = bind::fname(addr);
    res = MemValTy(mk<LAMBDA>(decl, ite));
    LOG("opsem.lambda", errs() << "MemCpy " << *res.v() << "\n");
  } else {
    DOG(ERR << "unsupported memcpy due to size and/or alignment.";);
    DOG(WARN << "Interpreting memcpy as noop");
    res = memRead;
  }
  return res;
}

OpSemMemRepr::MemValTy
OpSemMemLambdaRepr::MemCpy(PtrTy dPtr, PtrTy sPtr, unsigned len,
                           MemValTy memTrsfrRead, MemValTy memRead,
                           unsigned wordSzInBytes, PtrSortTy ptrSort,
                           uint32_t align) {
  MemValTy res = MemValTy(Expr());

  if (wordSzInBytes == 1 || (wordSzInBytes == 4 && align % 4 == 0) ||
      (wordSzInBytes == 8 && align % 4 == 0) ||
      m_memManager.isIgnoreAlignment()) {
    MemValTy srcMem = memTrsfrRead;

    if (len > 0) {
      unsigned lastAlignedBytePosToCopy;
      unsigned remainderBytes;
      if (m_memManager.isIgnoreAlignment()) {
        // if alignment is ignored, we treat it as alignment of 1
        lastAlignedBytePosToCopy = len - 1;
        remainderBytes = 0;
      } else {
        unsigned wordsToCopy = (len / wordSzInBytes);
        // -- -1 because ptrInRangeCheck is inclusive
        lastAlignedBytePosToCopy = (wordsToCopy - 1) * wordSzInBytes;
        remainderBytes = len % wordSzInBytes;
      }

      PtrTy dstLast = m_memManager.ptrAdd(dPtr, lastAlignedBytePosToCopy);

      PtrTy b0 = PtrTy(bind::bvar(0, ptrSort.toExpr()));
      Expr cmp = m_memManager.ptrInRangeCheck(dPtr, b0, dstLast);
      Expr offset = m_memManager.ptrOffsetFromBase(dPtr, sPtr);
      PtrTy readPtrInSrc = m_memManager.ptrAdd(b0, offset);

      Expr readFromSrc = op::bind::fapp(srcMem.toExpr(), readPtrInSrc.toExpr());
      Expr readFromDst = op::bind::fapp(memRead.toExpr(), b0.toExpr());

      // -- body of the new lambda function
      Expr body;
      if (remainderBytes) {
        LOG("opsem.lambda",
            WARN << "memcpy of incomplete words. potential bottleneck.");
        // -- if there are remainder bytes, stitch the last word together

        // -- address of last word in destination is after the last word copied
        PtrTy lastWordAddr =
            m_memManager.ptrAdd(dPtr, lastAlignedBytePosToCopy + wordSzInBytes);
        Expr isLastWordCmp = m_memManager.ptrEq(b0, lastWordAddr);

        // -- after compare, B0 is the same as last address
        Expr lastWordValDst = op::bind::fapp(memRead.toExpr(), b0.toExpr());
        // -- readPtrInSrc is an address in src that is at the corresponding
        // offset from B0
        Expr lastWordValSrc =
            op::bind::fapp(srcMem.toExpr(), readPtrInSrc.toExpr());

        // -- compute the last word by taking chunks of source and destination
        // -- words. source word comes first
        unsigned wordSzInBits = wordSzInBytes * 8;
        unsigned remainderBits = remainderBytes * 8;
        auto &alu = m_ctx.alu();
        Expr srcChunk =
            alu.Extract({lastWordValSrc, wordSzInBits}, 0, remainderBits - 1);
        Expr dstChunk = alu.Extract({lastWordValDst, wordSzInBits},
                                    remainderBits, wordSzInBits - 1);
        Expr lastWordVal = alu.Concat({dstChunk, wordSzInBits - remainderBits},
                                      {srcChunk, remainderBits});

        // -- construct the big ITE
        body = boolop::lite(isLastWordCmp, lastWordVal, readFromDst);
        body = boolop::lite(cmp, readFromSrc, body);
      } else {
        body = boolop::lite(cmp, readFromSrc, readFromDst);
      }

      // -- create lambda function by binding B0 to be the function argument
      Expr addr =
          bind::mkConst(mkTerm<std::string>("addr", m_efac), ptrSort.toExpr());
      Expr decl = bind::fname(addr);
      res = MemValTy(mk<LAMBDA>(decl, body));
      LOG("opsem.lambda", errs() << "MemCpy " << *res.v() << "\n");
    } else {
      // no-op
      res = memRead;
    }
  } else {
    LOG("opsem.lambda", errs() << "Word size and pointer are not aligned and "
                                  "alignment is not ignored!"
                               << "\n");
    DOG(WARN << "Interpreting memcpy as noop");
    res = memRead;
  }
  return res;
}

Expr OpSemMemLambdaRepr::coerceArrayToLambda(Expr arrVal) {
  assert(bind::isArrayConst(arrVal));

  Expr name = bind::fname(arrVal);
  Expr rTy = bind::rangeTy(name);
  Expr idxTy = sort::arrayIndexTy(rTy);

  Expr bvAddr = bind::mkConst(mkTerm<std::string>("addr", m_efac), idxTy);
  Expr sel = op::array::select(arrVal, bvAddr);

  /** lambda sel : arrVal[sel] **/
  return bind::abs<LAMBDA>(as_std_array(bvAddr), sel);
}

Expr OpSemMemLambdaRepr::makeLinearITE(PtrTy addr,
                                       const std::vector<PtrTy> &ptrKeys,
                                       const ExprVector &vals, Expr fallback) {
  assert(ptrKeys.size() == vals.size());

  Expr res = fallback;

  for (size_t i = ptrKeys.size() - 1; i < ptrKeys.size(); --i) {
    PtrTy k = ptrKeys[i];
    Expr v = vals[i];

    Expr cmp = m_memManager.ptrEq(addr, k);
    res = boolop::lite(cmp, v, res);
  }

  return res;
}

OpSemMemRepr::MemValTy OpSemMemLambdaRepr::MemFill(PtrTy dPtr, char *sPtr,
                                                   unsigned len, MemValTy mem,
                                                   unsigned wordSzInBytes,
                                                   PtrSortTy ptrSort,
                                                   uint32_t align) {
  (void)align;
  const unsigned sem_word_sz = wordSzInBytes;
  assert(sizeof(unsigned long) >= sem_word_sz);

  MemValTy initial = mem;
  LOG("opsem.lambda", errs() << "MemFill init: " << &initial << "\n");

  std::vector<PtrTy> ptrs;
  ptrs.reserve(len);
  ExprVector vals;
  vals.reserve(len);

  for (unsigned i = 0; i < len; i += sem_word_sz) {
    // copy bytes from buffer to word - word must accommodate largest
    // supported word size
    unsigned long word = 0;
    std::memcpy(&word, sPtr + i, sem_word_sz);
    Expr val = bv::bvnum(word, wordSzInBytes * m_BitsPerByte, m_efac);

    ptrs.push_back(m_memManager.ptrAdd(dPtr, i));
    vals.push_back(val);
  }

  PtrTy b0 = PtrTy(bind::bvar(0, ptrSort.toExpr()));
  Expr fallback = loadAlignedWordFromMem(b0, initial);
  Expr ite = makeLinearITE(b0, ptrs, vals, fallback);
  Expr addr =
      bind::mkConst(mkTerm<std::string>("addr", m_efac), ptrSort.toExpr());
  Expr decl = bind::fname(addr);
  Expr res = mk<LAMBDA>(decl, ite);

  LOG("opsem.lambda", errs() << "MemFill: " << *res << "\n");

  return MemValTy(res);
}
OpSemMemRepr::MemValTy OpSemMemLambdaRepr::FilledMemory(PtrSortTy ptrSort,
                                                        Expr v) {
  Expr addr =
      bind::mkConst(mkTerm<std::string>("addr", m_efac), ptrSort.toExpr());
  Expr decl = bind::fname(addr);
  // -- create constant lambda
  // lambda addr :: v
  return MemValTy(mk<LAMBDA>(decl, v));
}
} // namespace details
} // namespace seahorn
