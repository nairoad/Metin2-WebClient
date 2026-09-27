// SPDX-License-Identifier: GPL-2.0-or-later
// d3dx8_matrixstack.cpp - the D3DX matrix stack (`ID3DXMatrixStack`).

// Design:
// WHY. `CGraphicBase::PushMatrix`, `PopMatrix`, `MultMatrix`, `Rotate`,
// `Scale` and their siblings (EterLib/GrpBase.cpp) stand on
// `ID3DXMatrixStack`. It is a plain stack of matrices with operations that
// multiply from one side or the other - there is no graphics in it and
// nothing of Windows.
//
// `d3dx8.h` describes the interface from the measurement of the EterLib
// sources and leaves it without a body. The body is here.
//
// THE ONE THING THAT CAN BE CONFUSED HERE: THE SIDE OF THE MULTIPLICATION.
// Every operation has two forms - plain and "Local":
//
//   `MultMatrix(M)`      : top = top * M     ... the new one AT THE END
//   `MultMatrixLocal(M)` : top = M * top     ... the new one AT THE FRONT
//
// Matrix multiplication does not commute, so swapping the two gives a
// transform composed in the reverse order: a rotation about the world's
// origin instead of about the object's own axis. The model **does draw** -
// and flies all over the map.
//
// "Local" means "in the object's local frame", and that is exactly why the
// new matrix goes on the left: in Direct3D's row-vector notation a vector is
// multiplied BY a matrix from the left (`v * M`), so what is to act FIRST
// stands leftmost.

#include "win32_compat.h"
#include "d3dx8.h"

#include <vector>

namespace {

/// The stack. Never empty: the bottom is the identity and cannot be popped.
class CMatrixStack : public ID3DXMatrixStack
{
public:
    /// One reference; the stack holds a single identity matrix.
    CMatrixStack() : m_iRefs(1)
    {
        // The stack is never empty - `GetTop` on an empty stack would have
        // nothing to return, and Direct3D started from the identity.
        m_vecStack.resize(1);
        D3DXMatrixIdentity(&m_vecStack[0]);
    }

    /// Adds a reference; returns the new count.
    ULONG AddRef() override { return static_cast<ULONG>(++m_iRefs); }

    /// Drops a reference and deletes the stack at zero; returns the new count.
    ULONG Release() override
    {
        --m_iRefs;
        if (m_iRefs <= 0)
        {
            delete this;
            return 0;
        }
        return static_cast<ULONG>(m_iRefs);
    }

    /// Pushes a copy of the top; always D3D_OK.
    HRESULT Push() override
    {
        m_vecStack.push_back(m_vecStack.back());
        return D3D_OK;
    }

    /// Pops the top unless it is the last (identity bottom) matrix; always D3D_OK.
    HRESULT Pop() override
    {
        // Popping the last matrix would leave the stack empty. Direct3D
        // behaved the same: the bottom cannot be popped.
        if (m_vecStack.size() > 1)
            m_vecStack.pop_back();
        return D3D_OK;
    }

    /// Top = identity; D3D_OK.
    HRESULT LoadIdentity() override
    {
        D3DXMatrixIdentity(&m_vecStack.back());
        return D3D_OK;
    }

    /// Top = `*pM` (NULL ignored); D3D_OK.
    HRESULT LoadMatrix(CONST D3DXMATRIX* pM) override
    {
        if (pM)
            m_vecStack.back() = *pM;
        return D3D_OK;
    }

    /// Top = top * `*pM` (NULL ignored); D3D_OK.
    HRESULT MultMatrix(CONST D3DXMATRIX* pM) override
    {
        if (pM)
        {
            D3DXMATRIX k;
            D3DXMatrixMultiply(&k, &m_vecStack.back(), pM);
            m_vecStack.back() = k;
        }
        return D3D_OK;
    }

    /// Top = `*pM` * top (NULL ignored); D3D_OK.
    HRESULT MultMatrixLocal(CONST D3DXMATRIX* pM) override
    {
        if (pM)
        {
            // Reverse order - see the note at the top of the file.
            D3DXMATRIX k;
            D3DXMatrixMultiply(&k, pM, &m_vecStack.back());
            m_vecStack.back() = k;
        }
        return D3D_OK;
    }

    /// `MultMatrix(D3DXMatrixRotationAxis(pV, angle))`.
    HRESULT RotateAxis(CONST D3DXVECTOR3* pV, FLOAT angle) override
    {
        D3DXMATRIX k;
        D3DXMatrixRotationAxis(&k, pV, angle);
        return MultMatrix(&k);
    }

    /// `MultMatrixLocal(D3DXMatrixRotationAxis(pV, angle))`.
    HRESULT RotateAxisLocal(CONST D3DXVECTOR3* pV, FLOAT angle) override
    {
        D3DXMATRIX k;
        D3DXMatrixRotationAxis(&k, pV, angle);
        return MultMatrixLocal(&k);
    }

    /// `MultMatrix(D3DXMatrixRotationYawPitchRoll(yaw, pitch, roll))`.
    HRESULT RotateYawPitchRoll(FLOAT yaw, FLOAT pitch, FLOAT roll) override
    {
        D3DXMATRIX k;
        D3DXMatrixRotationYawPitchRoll(&k, yaw, pitch, roll);
        return MultMatrix(&k);
    }

    /// `MultMatrixLocal(D3DXMatrixRotationYawPitchRoll(yaw, pitch, roll))`.
    HRESULT RotateYawPitchRollLocal(FLOAT yaw, FLOAT pitch, FLOAT roll) override
    {
        D3DXMATRIX k;
        D3DXMatrixRotationYawPitchRoll(&k, yaw, pitch, roll);
        return MultMatrixLocal(&k);
    }

    /// `MultMatrix(D3DXMatrixScaling(x, y, z))`.
    HRESULT Scale(FLOAT x, FLOAT y, FLOAT z) override
    {
        D3DXMATRIX k;
        D3DXMatrixScaling(&k, x, y, z);
        return MultMatrix(&k);
    }

    /// `MultMatrixLocal(D3DXMatrixScaling(x, y, z))`.
    HRESULT ScaleLocal(FLOAT x, FLOAT y, FLOAT z) override
    {
        D3DXMATRIX k;
        D3DXMatrixScaling(&k, x, y, z);
        return MultMatrixLocal(&k);
    }

    /// `MultMatrix(D3DXMatrixTranslation(x, y, z))`.
    HRESULT Translate(FLOAT x, FLOAT y, FLOAT z) override
    {
        D3DXMATRIX k;
        D3DXMatrixTranslation(&k, x, y, z);
        return MultMatrix(&k);
    }

    /// `MultMatrixLocal(D3DXMatrixTranslation(x, y, z))`.
    HRESULT TranslateLocal(FLOAT x, FLOAT y, FLOAT z) override
    {
        D3DXMATRIX k;
        D3DXMatrixTranslation(&k, x, y, z);
        return MultMatrixLocal(&k);
    }

    /// The top matrix (never NULL - the stack is never empty).
    D3DXMATRIX* GetTop() override { return &m_vecStack.back(); }

private:
    /// Private: deleted only by `Release`.
    ~CMatrixStack() {}

    int m_iRefs;
    std::vector<D3DXMATRIX> m_vecStack;
};

}  // namespace

/// `D3DXCreateMatrixStack` of the original API; `CGraphicDevice::Create`
/// (grpdevice_gl.cpp) calls it once.
HRESULT D3DXCreateMatrixStack(DWORD /*Flags*/, LPD3DXMATRIXSTACK* ppStack)
{
    if (!ppStack)
        return E_FAIL;

    *ppStack = new CMatrixStack();
    return D3D_OK;
}
