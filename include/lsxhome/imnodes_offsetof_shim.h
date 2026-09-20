// Compatibility shim for the vendored ImNodes v0.5 translation unit.
//
// IM_OFFSETOF was exposed by Dear ImGui until 1.90 and removed in newer
// versions; ImNodes v0.5 still consumes it to build GStyleVarInfo. This header
// is force-included (/FI) into lsxhome_imnodes.cpp only, restoring the classic
// std::offsetof semantic without polluting any other TU.
//
// must not be passed as a /D define: cl.exe truncates function-like macro
// definitions at the comma, leaking garbage into <type_traits>.

#pragma once

#include <cstddef>

#ifndef IM_OFFSETOF
// NOTE: no leading "::" — MSVC would expand offsetof() to the compiler
// intrinsic __builtin_offsetof, and "::__builtin_offsetof" is a syntax error.
#define IM_OFFSETOF(_TYPE, _MEMBER) offsetof(_TYPE, _MEMBER)
#endif