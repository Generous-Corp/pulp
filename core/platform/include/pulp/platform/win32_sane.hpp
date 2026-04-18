#pragma once

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// WIN32_LEAN_AND_MEAN strips COM/OLE headers from <windows.h>. Include
// the COM base types explicitly so downstream TUs that pull in
// <UIAutomation.h>, <dwrite.h>, <dcomp.h>, etc. get IUnknown,
// ITextProvider, IDropTargetProvider, and friends with a consistent
// definition, not a partial forward-declaration that MSVC refuses to
// reconcile later.
//
// Without this, v0.15.0–v0.18.0's release-cli.yml failed on MSVC with
// C2146 / C2371 in UIAutomationCore.h. Followup to #383. See #384 for
// the full story and the PR-gate tightening.
#include <ObjBase.h>
#include <oleidl.h>
#endif
