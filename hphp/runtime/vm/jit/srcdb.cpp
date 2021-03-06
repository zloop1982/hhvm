/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-2014 Facebook, Inc. (http://www.facebook.com)     |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/
#include "hphp/runtime/vm/jit/srcdb.h"

#include <stdint.h>
#include <stdarg.h>
#include <string>

#include "hphp/util/trace.h"
#include "hphp/runtime/vm/jit/back-end-x64.h"
#include "hphp/runtime/vm/jit/service-requests-x64.h"
#include "hphp/runtime/vm/jit/mc-generator.h"
#include "hphp/runtime/vm/jit/service-requests-inline.h"
#include "hphp/runtime/vm/jit/relocation.h"

namespace HPHP { namespace jit {

TRACE_SET_MOD(trans)

void IncomingBranch::relocate(RelocationInfo& rel) {
  // compute adjustedTarget before altering the smash address,
  // because it might be a 5-byte nop
  TCA adjustedTarget = rel.adjustedAddressAfter(target());

  if (TCA adjusted = rel.adjustedAddressAfter(toSmash())) {
    m_ptr.set(m_ptr.tag(), adjusted);
  }

  if (adjustedTarget) {
    FTRACE_MOD(Trace::mcg, 1, "Patching: 0x{:08x} from 0x{:08x} to 0x{:08x}\n",
               (uintptr_t)toSmash(), (uintptr_t)target(),
               (uintptr_t)adjustedTarget);

    patch(adjustedTarget);
  }
}

void IncomingBranch::patch(TCA dest) {
  switch (type()) {
    case Tag::JMP: {
      mcg->backEnd().smashJmp(toSmash(), dest);
      mcg->getDebugInfo()->recordRelocMap(toSmash(), dest, "Arc-2");
      break;
    }

    case Tag::JCC: {
      mcg->backEnd().smashJcc(toSmash(), dest);
      mcg->getDebugInfo()->recordRelocMap(toSmash(), dest, "Arc-1");
      break;
    }

    case Tag::ADDR: {
      // Note that this effectively ignores a
      TCA* addr = reinterpret_cast<TCA*>(toSmash());
      assert_address_is_atomically_accessible(addr);
      *addr = dest;
      break;
    }
  }
}

TCA IncomingBranch::target() const {
  switch (type()) {
    case Tag::JMP:
      return mcg->backEnd().jmpTarget(toSmash());

    case Tag::JCC:
      return mcg->backEnd().jccTarget(toSmash());

    case Tag::ADDR:
      return *reinterpret_cast<TCA*>(toSmash());
  }
  always_assert(false);
}

void SrcRec::setFuncInfo(const Func* f) {
  m_unitMd5 = f->unit()->md5();
}

/*
 * The fallback translation is where to jump to if the
 * currently-translating translation's checks fail.
 *
 * The current heuristic we use for translation chaining is to assume
 * the most common cases are probably translated first, so we chain
 * new translations on the end.  This means if we have to fallback
 * from the currently-translating translation we jump to the "anchor"
 * translation (which just is a REQ_RETRANSLATE).
 */
TCA SrcRec::getFallbackTranslation() const {
  assertx(m_anchorTranslation);
  return m_anchorTranslation;
}

void SrcRec::chainFrom(IncomingBranch br) {
  assertx(br.type() == IncomingBranch::Tag::ADDR ||
         mcg->code.isValidCodeAddress(br.toSmash()));
  TCA destAddr = getTopTranslation();
  m_incomingBranches.push_back(br);
  TRACE(1, "SrcRec(%p)::chainFrom %p -> %p (type %d); %zd incoming branches\n",
        this,
        br.toSmash(), destAddr, br.type(), m_incomingBranches.size());
  br.patch(destAddr);
}

void SrcRec::emitFallbackJump(CodeBlock& cb, ConditionCode cc /* = -1 */) {
  // This is a spurious platform dependency. TODO(2990497)
  mcg->backEnd().prepareForSmash(
    cb,
    cc == CC_None ? x64::kJmpLen : x64::kJmpccLen
  );

  auto from = cb.frontier();
  TCA destAddr = getFallbackTranslation();
  mcg->backEnd().emitSmashableJump(cb, destAddr, cc);
  registerFallbackJump(from, cc);
}

void SrcRec::registerFallbackJump(TCA from, ConditionCode cc /* = -1 */) {
  auto incoming = cc < 0 ? IncomingBranch::jmpFrom(from)
                         : IncomingBranch::jccFrom(from);

  // We'll need to know the location of this jump later so we can
  // patch it to new translations added to the chain.
  mcg->cgFixups().m_inProgressTailJumps.push_back(incoming);
}

void SrcRec::emitFallbackJumpCustom(CodeBlock& cb, CodeBlock& frozen,
                                    SrcKey sk, TransFlags trflags,
                                    ConditionCode cc) {
  // Another platform dependency (the same one as above). TODO(2990497)
  auto toSmash = x64::emitRetranslate(cb, frozen, cc, sk, trflags);

  registerFallbackJump(toSmash, cc);
}

void SrcRec::newTranslation(TCA newStart,
                            GrowableVector<IncomingBranch>& tailBranches) {
  // When translation punts due to hitting limit, will generate one
  // more translation that will call the interpreter.
  assertx(m_translations.size() <= RuntimeOption::EvalJitMaxTranslations);

  TRACE(1, "SrcRec(%p)::newTranslation @%p, ", this, newStart);

  m_translations.push_back(newStart);
  if (!m_topTranslation.load(std::memory_order_acquire)) {
    m_topTranslation.store(newStart, std::memory_order_release);
    patchIncomingBranches(newStart);
  }

  /*
   * Link all the jumps from the current tail translation to this new
   * guy.
   *
   * It's (mostly) ok if someone is running in this code while we do
   * this: we hold the write lease, they'll instead jump to the anchor
   * and do REQ_RETRANSLATE and failing to get the write lease they'll
   * interp.  FIXME: Unfortunately, right now, in an unlikely race
   * another thread could create another translation with the same
   * type specialization that we just created in this case.  (If we
   * happen to release the write lease after they jump but before they
   * get into REQ_RETRANSLATE, they'll acquire it and generate a
   * translation possibly for this same situation.)
   */
  for (auto& br : m_tailFallbackJumps) {
    br.patch(newStart);
  }

  // This is the new tail translation, so store the fallback jump list
  // in case we translate this again.
  m_tailFallbackJumps.swap(tailBranches);
}

void SrcRec::relocate(RelocationInfo& rel) {
  if (auto adjusted = rel.adjustedAddressAfter(m_anchorTranslation)) {
    m_anchorTranslation = adjusted;
  }

  if (auto adjusted = rel.adjustedAddressAfter(m_topTranslation.load())) {
    m_topTranslation.store(adjusted);
  }

  for (auto &t : m_translations) {
    if (TCA adjusted = rel.adjustedAddressAfter(t)) {
      t = adjusted;
    }
  }

  for (auto &ib : m_tailFallbackJumps) {
    ib.relocate(rel);
  }

  for (auto &ib : m_incomingBranches) {
    ib.relocate(rel);
  }
}

void SrcRec::addDebuggerGuard(TCA dbgGuard, TCA dbgBranchGuardSrc) {
  assertx(!m_dbgBranchGuardSrc);

  TRACE(1, "SrcRec(%p)::addDebuggerGuard @%p, "
        "%zd incoming branches to rechain\n",
        this, dbgGuard, m_incomingBranches.size());

  patchIncomingBranches(dbgGuard);

  // Set m_dbgBranchGuardSrc after patching, so we don't try to patch
  // the debug guard.
  m_dbgBranchGuardSrc = dbgBranchGuardSrc;
  m_topTranslation.store(dbgGuard, std::memory_order_release);
}

void SrcRec::patchIncomingBranches(TCA newStart) {
  if (hasDebuggerGuard()) {
    // We have a debugger guard, so all jumps to us funnel through
    // this.  Just smash m_dbgBranchGuardSrc.
    TRACE(1, "smashing m_dbgBranchGuardSrc @%p\n", m_dbgBranchGuardSrc);
    mcg->backEnd().smashJmp(m_dbgBranchGuardSrc, newStart);
    return;
  }

  TRACE(1, "%zd incoming branches to rechain\n", m_incomingBranches.size());

  for (auto &br : m_incomingBranches) {
    TRACE(1, "SrcRec(%p)::newTranslation rechaining @%p -> %p\n",
          this, br.toSmash(), newStart);
    br.patch(newStart);
  }
}

void SrcRec::replaceOldTranslations() {
  // Everyone needs to give up on old translations; send them to the anchor,
  // which is a REQ_RETRANSLATE.
  m_translations.clear();
  m_tailFallbackJumps.clear();
  m_topTranslation.store(nullptr, std::memory_order_release);

  /*
   * It may seem a little weird that we're about to point every
   * incoming branch at the anchor, since that's going to just
   * unconditionally retranslate this SrcKey and never patch the
   * incoming branch to do something else.
   *
   * The reason this is ok is this mechanism is only used in
   * non-RepoAuthoritative mode, and the granularity of code
   * invalidation there is such that we'll only have incoming branches
   * like this basically within the same file since we don't have
   * whole program analysis.
   *
   * This means all these incoming branches are about to go away
   * anyway ...
   *
   * If we ever change that we'll have to change this to patch to
   * some sort of rebind requests.
   */
  assertx(!RuntimeOption::RepoAuthoritative || RuntimeOption::EvalJitPGO);
  patchIncomingBranches(m_anchorTranslation);
}

} } // HPHP::jit
