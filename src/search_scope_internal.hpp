#pragma once

#include "explorer/search.hpp"
#include <structuredquerycondition.h>

namespace explorer::search_scope_internal {

// Callers retain the original normalized/parsed scope rules. Only documented
// physical combinations whose native XML scope lost membership need a guard.
// Failed calls preserve all output arguments.
HRESULT requiresProtectiveGuard(const std::vector<SearchScopeRule>& rules, bool* required);

// A single native predicate: OR(include domains) AND NOT OR(exclude domains).
// A domain includes direct children, descendants only when recursive, and the
// excluded folder object itself. Leaves use canonical long filesystem paths
// and the native String semantic type; no matching-item snapshot is embedded.
HRESULT createScopeGuard(const std::vector<SearchScopeRule>& rules, ICondition** result);

// Compares every native leaf field, including semantic name and serialized
// PROPVARIANT. Only associative/idempotent same-connective normalization and
// singleton collapse are permitted; NOT and distinct AND/OR remain distinct.
HRESULT sameScopeGuard(ICondition* expected, ICondition* actual, bool* same);

}
