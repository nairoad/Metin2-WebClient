// PRTerrainLib/StdAfx.h - OUR header in place of the TMP4 one.
//
// It differs from the original by **one line**: it does not pull in `../scriptLib/StdAfx.h`.
//
// WHY that line is a problem: `scriptLib/StdAfx.h` pulls in not only
// `Python.h`, but also the **internal headers of the Python 2 parser** - `node.h`,
// `grammar.h`, `parsetok.h`, `symtable.h`, `marshal.h`. Most of them no longer
// exist in that form in Python 3.
//
// WHY it may be removed - this is a measurement, not convenience:
//   - `PRTerrainLib` uses **zero** symbols from Python;
//   - `GameLib` uses **zero** symbols from `ScriptLib`;
//   - of the whole `PRTerrainLib`, `GameLib` takes two classes: `CTerrainImpl`
//     and `CTextureSet`.
// So the include was accidental - a shared "stdafx" collecting everything
// that might ever come in handy.
//
// AND WHY it matters more broadly: the `Python-2.7` headers from
// `extern/include` are **a dead end**: the client embeds CPython 3 built for wasm.
// There is no point building Python 2.7 for emscripten - it has to be moved out
// of the way, and the bindings written anew for Python 3.

#if !defined(AFX_STDAFX_H__EECC0C4D_07A5_4D9E_B40F_767A80FD6DE6__INCLUDED_)
#define AFX_STDAFX_H__EECC0C4D_07A5_4D9E_B40F_767A80FD6DE6__INCLUDED_

#define WIN32_LEAN_AND_MEAN

#include "../EterLib/StdAfx.h"
#include "../EterGrnLib/StdAfx.h"

// These three were originally pulled in by `scriptLib/StdAfx.h` - not through Python, but
// "on the way": `scriptLib/Resource.h` took `eterLib/ResourceManager.h`,
// and `effectLib/StdAfx.h` (through `scriptLib`) took `eterBase/Timer.h`.
//
// The first time I cut out only the Python line and **narrowed more than
// I meant to** - `Terrain.cpp` lost `ELTimer_GetMSec`, and `TextureSet.cpp`
// `CResourceManager` and `LoadMultipleTextData`. The measurement caught it,
// not I while writing. So I list them **explicitly**, instead of counting on them
// arriving through the chain - that way it shows what this file really needs.
#include "../eterBase/Timer.h"
#include "../eterLib/ResourceManager.h"
#include "../eterLib/Util.h"

/* Fast float <-> int conversion */
extern float PR_FCNV;
extern long  PR_ICNV;

#endif  // !defined(AFX_STDAFX_H__...)
