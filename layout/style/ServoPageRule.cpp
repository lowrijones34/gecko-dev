/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* representation of CSSPageRule for stylo */

#include "mozilla/ServoPageRule.h"

#include "mozilla/DeclarationBlock.h"
#include "mozilla/ServoBindings.h"

using namespace mozilla::dom;

namespace mozilla {

// -- ServoPageRuleDeclaration ---------------------------------------

ServoPageRuleDeclaration::ServoPageRuleDeclaration(
  already_AddRefed<RawServoDeclarationBlock> aDecls)
  : mDecls(new DeclarationBlock(std::move(aDecls)))
{
}

ServoPageRuleDeclaration::~ServoPageRuleDeclaration()
{
  mDecls->SetOwningRule(nullptr);
}

// QueryInterface implementation for ServoPageRuleDeclaration
NS_INTERFACE_MAP_BEGIN(ServoPageRuleDeclaration)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  // We forward the cycle collection interfaces to Rule(), which is
  // never null (in fact, we're part of that object!)
  if (aIID.Equals(NS_GET_IID(nsCycleCollectionISupports)) ||
      aIID.Equals(NS_GET_IID(nsXPCOMCycleCollectionParticipant))) {
    return Rule()->QueryInterface(aIID, aInstancePtr);
  }
  else
NS_IMPL_QUERY_TAIL_INHERITING(nsDOMCSSDeclaration)

NS_IMPL_ADDREF_USING_AGGREGATOR(ServoPageRuleDeclaration, Rule())
NS_IMPL_RELEASE_USING_AGGREGATOR(ServoPageRuleDeclaration, Rule())

/* nsDOMCSSDeclaration implementation */

css::Rule*
ServoPageRuleDeclaration::GetParentRule()
{
  return Rule();
}

nsINode*
ServoPageRuleDeclaration::GetParentObject()
{
  return Rule()->GetParentObject();
}

DeclarationBlock*
ServoPageRuleDeclaration::GetCSSDeclaration(Operation aOperation)
{
  return mDecls;
}

nsresult
ServoPageRuleDeclaration::SetCSSDeclaration(DeclarationBlock* aDecl)
{
  MOZ_ASSERT(aDecl, "must be non-null");
  ServoPageRule* rule = Rule();

  if (aDecl != mDecls) {
    mDecls->SetOwningRule(nullptr);
    RefPtr<DeclarationBlock> decls = aDecl;
    Servo_PageRule_SetStyle(rule->Raw(), decls->Raw());
    mDecls = decls.forget();
    mDecls->SetOwningRule(rule);
  }

  return NS_OK;
}

nsIDocument*
ServoPageRuleDeclaration::DocToUpdate()
{
  return nullptr;
}

nsDOMCSSDeclaration::ParsingEnvironment
ServoPageRuleDeclaration::GetParsingEnvironment(
  nsIPrincipal* aSubjectPrincipal) const
{
  return GetParsingEnvironmentForRule(Rule());
}

// -- ServoPageRule --------------------------------------------------

ServoPageRule::ServoPageRule(RefPtr<RawServoPageRule> aRawRule,
                             uint32_t aLine, uint32_t aColumn)
  : CSSPageRule(aLine, aColumn)
  , mRawRule(std::move(aRawRule))
  , mDecls(Servo_PageRule_GetStyle(mRawRule).Consume())
{
}

ServoPageRule::~ServoPageRule()
{
}

NS_IMPL_ADDREF_INHERITED(ServoPageRule, CSSPageRule)
NS_IMPL_RELEASE_INHERITED(ServoPageRule, CSSPageRule)

// QueryInterface implementation for PageRule
NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(ServoPageRule)
NS_INTERFACE_MAP_END_INHERITING(CSSPageRule)

NS_IMPL_CYCLE_COLLECTION_CLASS(ServoPageRule)

NS_IMPL_CYCLE_COLLECTION_TRACE_BEGIN_INHERITED(ServoPageRule, CSSPageRule)
  // Keep this in sync with IsCCLeaf.

  // Trace the wrapper for our declaration.  This just expands out
  // NS_IMPL_CYCLE_COLLECTION_TRACE_PRESERVED_WRAPPER which we can't use
  // directly because the wrapper is on the declaration, not on us.
  tmp->mDecls.TraceWrapper(aCallbacks, aClosure);
NS_IMPL_CYCLE_COLLECTION_TRACE_END

NS_IMPL_CYCLE_COLLECTION_UNLINK_BEGIN_INHERITED(ServoPageRule, CSSPageRule)
  // Keep this in sync with IsCCLeaf.

  // Unlink the wrapper for our declaraton.  This just expands out
  // NS_IMPL_CYCLE_COLLECTION_UNLINK_PRESERVED_WRAPPER which we can't use
  // directly because the wrapper is on the declaration, not on us.
  tmp->mDecls.ReleaseWrapper(static_cast<nsISupports*>(p));
  tmp->mDecls.mDecls->SetOwningRule(nullptr);
NS_IMPL_CYCLE_COLLECTION_UNLINK_END

NS_IMPL_CYCLE_COLLECTION_TRAVERSE_BEGIN_INHERITED(ServoPageRule, CSSPageRule)
  // Keep this in sync with IsCCLeaf.
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_END

bool
ServoPageRule::IsCCLeaf() const
{
  if (!Rule::IsCCLeaf()) {
    return false;
  }

  return !mDecls.PreservingWrapper();
}

size_t
ServoPageRule::SizeOfIncludingThis(MallocSizeOf aMallocSizeOf) const
{
  // TODO Implement this!
  return aMallocSizeOf(this);
}

#ifdef DEBUG
void
ServoPageRule::List(FILE* out, int32_t aIndent) const
{
  nsAutoCString str;
  for (int32_t i = 0; i < aIndent; i++) {
    str.AppendLiteral("  ");
  }
  Servo_PageRule_Debug(mRawRule, &str);
  fprintf_stderr(out, "%s\n", str.get());
}
#endif

/* CSSRule implementation */

void
ServoPageRule::GetCssText(nsAString& aCssText) const
{
  Servo_PageRule_GetCssText(mRawRule, &aCssText);
}

/* CSSPageRule implementation */

nsICSSDeclaration*
ServoPageRule::Style()
{
  return &mDecls;
}

} // namespace mozilla
