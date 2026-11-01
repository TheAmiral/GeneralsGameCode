/*
**	Armies Of Tomorrow: WW3
**	Copyright 2026 Armies Of Tomorrow contributors
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// AoT @feature 64-bit: replacement for the D3DX8 functions the engine uses.
//
// Microsoft never shipped D3DX8 for 64-bit, so a 64-bit build cannot link the
// d3dx8.lib from the DX8 SDK. This file implements only the D3DX8 entry points
// that GeneralsMD and Core call (see README.md for the list and the call sites).
//
// The math functions follow the D3DX conventions: row-major matrices and row
// vectors (v' = v * M). The texture functions work on lockable surfaces of the
// uncompressed formats the engine creates. D3DXAssembleShader forwards to the
// 64-bit D3DX9 runtime (d3dx9_43.dll) when it is present.
//
// This library is only built for 64-bit targets. 32-bit builds keep linking the
// original d3dx8.lib, so retail-compatible builds are not affected.

#include <windows.h>
#include <d3d8.h>
#include <d3dx8.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Math
// ---------------------------------------------------------------------------

extern "C" D3DXMATRIX* WINAPI D3DXMatrixTranspose(D3DXMATRIX* pOut, CONST D3DXMATRIX* pM)
{
	D3DXMATRIX t;
	for (int r = 0; r < 4; ++r)
		for (int c = 0; c < 4; ++c)
			t.m[r][c] = pM->m[c][r];
	*pOut = t;
	return pOut;
}

extern "C" D3DXMATRIX* WINAPI D3DXMatrixMultiply(D3DXMATRIX* pOut, CONST D3DXMATRIX* pM1, CONST D3DXMATRIX* pM2)
{
	D3DXMATRIX t;
	for (int r = 0; r < 4; ++r)
	{
		for (int c = 0; c < 4; ++c)
		{
			t.m[r][c] = pM1->m[r][0] * pM2->m[0][c]
			          + pM1->m[r][1] * pM2->m[1][c]
			          + pM1->m[r][2] * pM2->m[2][c]
			          + pM1->m[r][3] * pM2->m[3][c];
		}
	}
	*pOut = t;
	return pOut;
}

extern "C" D3DXMATRIX* WINAPI D3DXMatrixMultiplyTranspose(D3DXMATRIX* pOut, CONST D3DXMATRIX* pM1, CONST D3DXMATRIX* pM2)
{
	D3DXMatrixMultiply(pOut, pM1, pM2);
	return D3DXMatrixTranspose(pOut, pOut);
}

extern "C" FLOAT WINAPI D3DXMatrixfDeterminant(CONST D3DXMATRIX* pM)
{
	const float (*m)[4] = pM->m;
	const float s0 = m[0][0] * m[1][1] - m[1][0] * m[0][1];
	const float s1 = m[0][0] * m[1][2] - m[1][0] * m[0][2];
	const float s2 = m[0][0] * m[1][3] - m[1][0] * m[0][3];
	const float s3 = m[0][1] * m[1][2] - m[1][1] * m[0][2];
	const float s4 = m[0][1] * m[1][3] - m[1][1] * m[0][3];
	const float s5 = m[0][2] * m[1][3] - m[1][2] * m[0][3];
	const float c5 = m[2][2] * m[3][3] - m[3][2] * m[2][3];
	const float c4 = m[2][1] * m[3][3] - m[3][1] * m[2][3];
	const float c3 = m[2][1] * m[3][2] - m[3][1] * m[2][2];
	const float c2 = m[2][0] * m[3][3] - m[3][0] * m[2][3];
	const float c1 = m[2][0] * m[3][2] - m[3][0] * m[2][2];
	const float c0 = m[2][0] * m[3][1] - m[3][0] * m[2][1];
	return s0 * c5 - s1 * c4 + s2 * c3 + s3 * c2 - s4 * c1 + s5 * c0;
}

extern "C" D3DXMATRIX* WINAPI D3DXMatrixInverse(D3DXMATRIX* pOut, FLOAT* pDeterminant, CONST D3DXMATRIX* pM)
{
	const float (*m)[4] = pM->m;
	const float s0 = m[0][0] * m[1][1] - m[1][0] * m[0][1];
	const float s1 = m[0][0] * m[1][2] - m[1][0] * m[0][2];
	const float s2 = m[0][0] * m[1][3] - m[1][0] * m[0][3];
	const float s3 = m[0][1] * m[1][2] - m[1][1] * m[0][2];
	const float s4 = m[0][1] * m[1][3] - m[1][1] * m[0][3];
	const float s5 = m[0][2] * m[1][3] - m[1][2] * m[0][3];
	const float c5 = m[2][2] * m[3][3] - m[3][2] * m[2][3];
	const float c4 = m[2][1] * m[3][3] - m[3][1] * m[2][3];
	const float c3 = m[2][1] * m[3][2] - m[3][1] * m[2][2];
	const float c2 = m[2][0] * m[3][3] - m[3][0] * m[2][3];
	const float c1 = m[2][0] * m[3][2] - m[3][0] * m[2][2];
	const float c0 = m[2][0] * m[3][1] - m[3][0] * m[2][1];

	const float det = s0 * c5 - s1 * c4 + s2 * c3 + s3 * c2 - s4 * c1 + s5 * c0;
	if (pDeterminant)
		*pDeterminant = det;
	if (det == 0.0f)
		return nullptr;

	const float inv = 1.0f / det;
	D3DXMATRIX r;
	r.m[0][0] = ( m[1][1] * c5 - m[1][2] * c4 + m[1][3] * c3) * inv;
	r.m[0][1] = (-m[0][1] * c5 + m[0][2] * c4 - m[0][3] * c3) * inv;
	r.m[0][2] = ( m[3][1] * s5 - m[3][2] * s4 + m[3][3] * s3) * inv;
	r.m[0][3] = (-m[2][1] * s5 + m[2][2] * s4 - m[2][3] * s3) * inv;
	r.m[1][0] = (-m[1][0] * c5 + m[1][2] * c2 - m[1][3] * c1) * inv;
	r.m[1][1] = ( m[0][0] * c5 - m[0][2] * c2 + m[0][3] * c1) * inv;
	r.m[1][2] = (-m[3][0] * s5 + m[3][2] * s2 - m[3][3] * s1) * inv;
	r.m[1][3] = ( m[2][0] * s5 - m[2][2] * s2 + m[2][3] * s1) * inv;
	r.m[2][0] = ( m[1][0] * c4 - m[1][1] * c2 + m[1][3] * c0) * inv;
	r.m[2][1] = (-m[0][0] * c4 + m[0][1] * c2 - m[0][3] * c0) * inv;
	r.m[2][2] = ( m[3][0] * s4 - m[3][1] * s2 + m[3][3] * s0) * inv;
	r.m[2][3] = (-m[2][0] * s4 + m[2][1] * s2 - m[2][3] * s0) * inv;
	r.m[3][0] = (-m[1][0] * c3 + m[1][1] * c1 - m[1][2] * c0) * inv;
	r.m[3][1] = ( m[0][0] * c3 - m[0][1] * c1 + m[0][2] * c0) * inv;
	r.m[3][2] = (-m[3][0] * s3 + m[3][1] * s1 - m[3][2] * s0) * inv;
	r.m[3][3] = ( m[2][0] * s3 - m[2][1] * s1 + m[2][2] * s0) * inv;
	*pOut = r;
	return pOut;
}

static void SetIdentity(D3DXMATRIX* pOut)
{
	memset(pOut->m, 0, sizeof(pOut->m));
	pOut->m[0][0] = pOut->m[1][1] = pOut->m[2][2] = pOut->m[3][3] = 1.0f;
}

extern "C" D3DXMATRIX* WINAPI D3DXMatrixScaling(D3DXMATRIX* pOut, FLOAT sx, FLOAT sy, FLOAT sz)
{
	SetIdentity(pOut);
	pOut->m[0][0] = sx;
	pOut->m[1][1] = sy;
	pOut->m[2][2] = sz;
	return pOut;
}

extern "C" D3DXMATRIX* WINAPI D3DXMatrixTranslation(D3DXMATRIX* pOut, FLOAT x, FLOAT y, FLOAT z)
{
	SetIdentity(pOut);
	pOut->m[3][0] = x;
	pOut->m[3][1] = y;
	pOut->m[3][2] = z;
	return pOut;
}

extern "C" D3DXMATRIX* WINAPI D3DXMatrixRotationX(D3DXMATRIX* pOut, FLOAT Angle)
{
	const float s = sinf(Angle), c = cosf(Angle);
	SetIdentity(pOut);
	pOut->m[1][1] = c;  pOut->m[1][2] = s;
	pOut->m[2][1] = -s; pOut->m[2][2] = c;
	return pOut;
}

extern "C" D3DXMATRIX* WINAPI D3DXMatrixRotationY(D3DXMATRIX* pOut, FLOAT Angle)
{
	const float s = sinf(Angle), c = cosf(Angle);
	SetIdentity(pOut);
	pOut->m[0][0] = c; pOut->m[0][2] = -s;
	pOut->m[2][0] = s; pOut->m[2][2] = c;
	return pOut;
}

extern "C" D3DXMATRIX* WINAPI D3DXMatrixRotationZ(D3DXMATRIX* pOut, FLOAT Angle)
{
	const float s = sinf(Angle), c = cosf(Angle);
	SetIdentity(pOut);
	pOut->m[0][0] = c;  pOut->m[0][1] = s;
	pOut->m[1][0] = -s; pOut->m[1][1] = c;
	return pOut;
}

extern "C" D3DXVECTOR4* WINAPI D3DXVec4Transform(D3DXVECTOR4* pOut, CONST D3DXVECTOR4* pV, CONST D3DXMATRIX* pM)
{
	const float x = pV->x, y = pV->y, z = pV->z, w = pV->w;
	D3DXVECTOR4 r;
	r.x = x * pM->m[0][0] + y * pM->m[1][0] + z * pM->m[2][0] + w * pM->m[3][0];
	r.y = x * pM->m[0][1] + y * pM->m[1][1] + z * pM->m[2][1] + w * pM->m[3][1];
	r.z = x * pM->m[0][2] + y * pM->m[1][2] + z * pM->m[2][2] + w * pM->m[3][2];
	r.w = x * pM->m[0][3] + y * pM->m[1][3] + z * pM->m[2][3] + w * pM->m[3][3];
	*pOut = r;
	return pOut;
}

extern "C" D3DXVECTOR4* WINAPI D3DXVec3Transform(D3DXVECTOR4* pOut, CONST D3DXVECTOR3* pV, CONST D3DXMATRIX* pM)
{
	const D3DXVECTOR4 v(pV->x, pV->y, pV->z, 1.0f);
	return D3DXVec4Transform(pOut, &v, pM);
}

// ---------------------------------------------------------------------------
// Vertex format and errors
// ---------------------------------------------------------------------------

extern "C" UINT WINAPI D3DXGetFVFVertexSize(DWORD FVF)
{
	UINT size = 0;
	switch (FVF & D3DFVF_POSITION_MASK)
	{
	case D3DFVF_XYZ:    size += 12; break;
	case D3DFVF_XYZRHW: size += 16; break;
	case D3DFVF_XYZB1:  size += 16; break;
	case D3DFVF_XYZB2:  size += 20; break;
	case D3DFVF_XYZB3:  size += 24; break;
	case D3DFVF_XYZB4:  size += 28; break;
	case D3DFVF_XYZB5:  size += 32; break;
	default: break;
	}
	if (FVF & D3DFVF_NORMAL)   size += 12;
	if (FVF & D3DFVF_PSIZE)    size += 4;
	if (FVF & D3DFVF_DIFFUSE)  size += 4;
	if (FVF & D3DFVF_SPECULAR) size += 4;

	const UINT texCount = (FVF & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
	for (UINT i = 0; i < texCount; ++i)
	{
		// D3DFVF_TEXCOORDSIZEn: 0 = 2 floats, 1 = 3 floats, 2 = 4 floats, 3 = 1 float.
		static const UINT floats[4] = { 2, 3, 4, 1 };
		size += floats[(FVF >> (16 + i * 2)) & 3] * 4;
	}
	return size;
}

extern "C" HRESULT WINAPI D3DXGetErrorStringA(HRESULT hr, LPSTR pBuffer, UINT BufferLen)
{
	if (pBuffer == nullptr || BufferLen == 0)
		return D3DERR_INVALIDCALL;
	snprintf(pBuffer, BufferLen, "HRESULT 0x%08lX", (unsigned long)hr);
	return D3D_OK;
}

// ---------------------------------------------------------------------------
// Textures
// ---------------------------------------------------------------------------

static UINT FullMipCount(UINT w, UINT h, UINT d = 1)
{
	UINT levels = 1;
	while (w > 1 || h > 1 || d > 1)
	{
		w = w > 1 ? w / 2 : 1;
		h = h > 1 ? h / 2 : 1;
		d = d > 1 ? d / 2 : 1;
		++levels;
	}
	return levels;
}

static UINT DefaultSize(UINT v)
{
	return (v == 0 || v == D3DX_DEFAULT) ? 256 : v;
}

static UINT DefaultLevels(UINT levels)
{
	// D3DX8 treats 0 and D3DX_DEFAULT as "full chain", which is 0 for D3D8.
	return (levels == D3DX_DEFAULT) ? 0 : levels;
}

extern "C" HRESULT WINAPI D3DXCreateTexture(LPDIRECT3DDEVICE8 pDevice, UINT Width, UINT Height, UINT MipLevels,
	DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, LPDIRECT3DTEXTURE8* ppTexture)
{
	if (pDevice == nullptr || ppTexture == nullptr)
		return D3DERR_INVALIDCALL;
	if (Format == D3DFMT_UNKNOWN)
		Format = D3DFMT_A8R8G8B8;
	return pDevice->CreateTexture(DefaultSize(Width), DefaultSize(Height), DefaultLevels(MipLevels), Usage, Format, Pool, ppTexture);
}

extern "C" HRESULT WINAPI D3DXCreateCubeTexture(LPDIRECT3DDEVICE8 pDevice, UINT Size, UINT MipLevels,
	DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, LPDIRECT3DCUBETEXTURE8* ppCubeTexture)
{
	if (pDevice == nullptr || ppCubeTexture == nullptr)
		return D3DERR_INVALIDCALL;
	if (Format == D3DFMT_UNKNOWN)
		Format = D3DFMT_A8R8G8B8;
	return pDevice->CreateCubeTexture(DefaultSize(Size), DefaultLevels(MipLevels), Usage, Format, Pool, ppCubeTexture);
}

extern "C" HRESULT WINAPI D3DXCreateVolumeTexture(LPDIRECT3DDEVICE8 pDevice, UINT Width, UINT Height, UINT Depth,
	UINT MipLevels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, LPDIRECT3DVOLUMETEXTURE8* ppVolumeTexture)
{
	if (pDevice == nullptr || ppVolumeTexture == nullptr)
		return D3DERR_INVALIDCALL;
	if (Format == D3DFMT_UNKNOWN)
		Format = D3DFMT_A8R8G8B8;
	const UINT depth = (Depth == 0 || Depth == D3DX_DEFAULT) ? 1 : Depth;
	return pDevice->CreateVolumeTexture(DefaultSize(Width), DefaultSize(Height), depth, DefaultLevels(MipLevels), Usage, Format, Pool, ppVolumeTexture);
}

// File based texture creation is only a fallback path in dx8wrapper.cpp (the engine
// loads textures through TextureLoader). Failing here makes it use the missing texture.
extern "C" HRESULT WINAPI D3DXCreateTextureFromFileExA(LPDIRECT3DDEVICE8, LPCSTR, UINT, UINT, UINT, DWORD,
	D3DFORMAT, D3DPOOL, DWORD, DWORD, D3DCOLOR, D3DXIMAGE_INFO*, PALETTEENTRY*, LPDIRECT3DTEXTURE8* ppTexture)
{
	if (ppTexture)
		*ppTexture = nullptr;
	return D3DERR_NOTAVAILABLE;
}

extern "C" HRESULT WINAPI D3DXLoadSurfaceFromFileA(LPDIRECT3DSURFACE8, CONST PALETTEENTRY*, CONST RECT*, LPCSTR,
	CONST RECT*, DWORD, D3DCOLOR, D3DXIMAGE_INFO*)
{
	return D3DERR_NOTAVAILABLE;
}

namespace
{

struct Color
{
	float a, r, g, b;
};

// Bytes per pixel for the uncompressed formats this file can convert, 0 otherwise.
UINT BytesPerPixel(D3DFORMAT f)
{
	switch (f)
	{
	case D3DFMT_A8R8G8B8:
	case D3DFMT_X8R8G8B8: return 4;
	case D3DFMT_R8G8B8:   return 3;
	case D3DFMT_R5G6B5:
	case D3DFMT_X1R5G5B5:
	case D3DFMT_A1R5G5B5:
	case D3DFMT_A4R4G4B4:
	case D3DFMT_X4R4G4B4:
	case D3DFMT_A8R3G3B2:
	case D3DFMT_A8L8:     return 2;
	case D3DFMT_R3G3B2:
	case D3DFMT_A8:
	case D3DFMT_L8:
	case D3DFMT_A4L4:     return 1;
	default:              return 0;
	}
}

bool IsCompressed(D3DFORMAT f)
{
	return f == D3DFMT_DXT1 || f == D3DFMT_DXT2 || f == D3DFMT_DXT3 || f == D3DFMT_DXT4 || f == D3DFMT_DXT5;
}

UINT BlockBytes(D3DFORMAT f)
{
	return f == D3DFMT_DXT1 ? 8 : 16;
}

inline float Bits(DWORD v, int shift, int bits)
{
	const DWORD max = (1u << bits) - 1;
	return float((v >> shift) & max) / float(max);
}

inline DWORD Pack(float c, int shift, int bits)
{
	const DWORD max = (1u << bits) - 1;
	if (c < 0.0f) c = 0.0f;
	if (c > 1.0f) c = 1.0f;
	return (DWORD(c * float(max) + 0.5f) & max) << shift;
}

DWORD ReadRaw(const BYTE* p, UINT bpp)
{
	switch (bpp)
	{
	case 4: return *(const DWORD*)p;
	case 3: return DWORD(p[0]) | (DWORD(p[1]) << 8) | (DWORD(p[2]) << 16);
	case 2: return *(const WORD*)p;
	default: return *p;
	}
}

void WriteRaw(BYTE* p, UINT bpp, DWORD v)
{
	switch (bpp)
	{
	case 4: *(DWORD*)p = v; break;
	case 3: p[0] = BYTE(v); p[1] = BYTE(v >> 8); p[2] = BYTE(v >> 16); break;
	case 2: *(WORD*)p = WORD(v); break;
	default: *p = BYTE(v); break;
	}
}

Color Decode(D3DFORMAT f, DWORD v)
{
	Color c = { 1.0f, 0.0f, 0.0f, 0.0f };
	switch (f)
	{
	case D3DFMT_A8R8G8B8: c.a = Bits(v, 24, 8); // fall through
	case D3DFMT_X8R8G8B8:
	case D3DFMT_R8G8B8:   c.r = Bits(v, 16, 8); c.g = Bits(v, 8, 8); c.b = Bits(v, 0, 8); break;
	case D3DFMT_R5G6B5:   c.r = Bits(v, 11, 5); c.g = Bits(v, 5, 6); c.b = Bits(v, 0, 5); break;
	case D3DFMT_A1R5G5B5: c.a = Bits(v, 15, 1); // fall through
	case D3DFMT_X1R5G5B5: c.r = Bits(v, 10, 5); c.g = Bits(v, 5, 5); c.b = Bits(v, 0, 5); break;
	case D3DFMT_A4R4G4B4: c.a = Bits(v, 12, 4); // fall through
	case D3DFMT_X4R4G4B4: c.r = Bits(v, 8, 4); c.g = Bits(v, 4, 4); c.b = Bits(v, 0, 4); break;
	case D3DFMT_A8R3G3B2: c.a = Bits(v, 8, 8); // fall through
	case D3DFMT_R3G3B2:   c.r = Bits(v, 5, 3); c.g = Bits(v, 2, 3); c.b = Bits(v, 0, 2); break;
	case D3DFMT_A8:       c.a = Bits(v, 0, 8); break;
	case D3DFMT_L8:       c.r = c.g = c.b = Bits(v, 0, 8); break;
	case D3DFMT_A8L8:     c.a = Bits(v, 8, 8); c.r = c.g = c.b = Bits(v, 0, 8); break;
	case D3DFMT_A4L4:     c.a = Bits(v, 4, 4); c.r = c.g = c.b = Bits(v, 0, 4); break;
	default: break;
	}
	return c;
}

DWORD Encode(D3DFORMAT f, const Color& c)
{
	const float l = c.r * 0.299f + c.g * 0.587f + c.b * 0.114f;
	switch (f)
	{
	case D3DFMT_A8R8G8B8: return Pack(c.a, 24, 8) | Pack(c.r, 16, 8) | Pack(c.g, 8, 8) | Pack(c.b, 0, 8);
	case D3DFMT_X8R8G8B8: return 0xFF000000u | Pack(c.r, 16, 8) | Pack(c.g, 8, 8) | Pack(c.b, 0, 8);
	case D3DFMT_R8G8B8:   return Pack(c.r, 16, 8) | Pack(c.g, 8, 8) | Pack(c.b, 0, 8);
	case D3DFMT_R5G6B5:   return Pack(c.r, 11, 5) | Pack(c.g, 5, 6) | Pack(c.b, 0, 5);
	case D3DFMT_A1R5G5B5: return Pack(c.a, 15, 1) | Pack(c.r, 10, 5) | Pack(c.g, 5, 5) | Pack(c.b, 0, 5);
	case D3DFMT_X1R5G5B5: return 0x8000u | Pack(c.r, 10, 5) | Pack(c.g, 5, 5) | Pack(c.b, 0, 5);
	case D3DFMT_A4R4G4B4: return Pack(c.a, 12, 4) | Pack(c.r, 8, 4) | Pack(c.g, 4, 4) | Pack(c.b, 0, 4);
	case D3DFMT_X4R4G4B4: return 0xF000u | Pack(c.r, 8, 4) | Pack(c.g, 4, 4) | Pack(c.b, 0, 4);
	case D3DFMT_A8R3G3B2: return Pack(c.a, 8, 8) | Pack(c.r, 5, 3) | Pack(c.g, 2, 3) | Pack(c.b, 0, 2);
	case D3DFMT_R3G3B2:   return Pack(c.r, 5, 3) | Pack(c.g, 2, 3) | Pack(c.b, 0, 2);
	case D3DFMT_A8:       return Pack(c.a, 0, 8);
	case D3DFMT_L8:       return Pack(l, 0, 8);
	case D3DFMT_A8L8:     return Pack(c.a, 8, 8) | Pack(l, 0, 8);
	case D3DFMT_A4L4:     return Pack(c.a, 4, 4) | Pack(l, 0, 4);
	default:              return 0;
	}
}

RECT FullRect(const D3DSURFACE_DESC& d)
{
	RECT r = { 0, 0, LONG(d.Width), LONG(d.Height) };
	return r;
}

} // namespace

extern "C" HRESULT WINAPI D3DXLoadSurfaceFromSurface(LPDIRECT3DSURFACE8 pDestSurface, CONST PALETTEENTRY* pDestPalette,
	CONST RECT* pDestRect, LPDIRECT3DSURFACE8 pSrcSurface, CONST PALETTEENTRY* pSrcPalette, CONST RECT* pSrcRect,
	DWORD Filter, D3DCOLOR ColorKey)
{
	(void)pDestPalette;
	(void)pSrcPalette;
	if (pDestSurface == nullptr || pSrcSurface == nullptr)
		return D3DERR_INVALIDCALL;

	D3DSURFACE_DESC dd, sd;
	if (FAILED(pDestSurface->GetDesc(&dd)) || FAILED(pSrcSurface->GetDesc(&sd)))
		return D3DERR_INVALIDCALL;

	const RECT dr = pDestRect ? *pDestRect : FullRect(dd);
	const RECT sr = pSrcRect ? *pSrcRect : FullRect(sd);
	const UINT dw = UINT(dr.right - dr.left), dh = UINT(dr.bottom - dr.top);
	const UINT sw = UINT(sr.right - sr.left), sh = UINT(sr.bottom - sr.top);
	if (dw == 0 || dh == 0 || sw == 0 || sh == 0)
		return D3D_OK;

	const bool sameShape = dd.Format == sd.Format && dw == sw && dh == sh;
	const bool compressed = IsCompressed(dd.Format) || IsCompressed(sd.Format);
	if (compressed && !sameShape)
	{
		// Converting or scaling block-compressed data is not needed by the engine.
		return D3DERR_NOTAVAILABLE;
	}

	const UINT sbpp = BytesPerPixel(sd.Format);
	const UINT dbpp = BytesPerPixel(dd.Format);
	if (!compressed && (sbpp == 0 || dbpp == 0))
		return D3DERR_NOTAVAILABLE;

	D3DLOCKED_RECT sl, dl;
	if (FAILED(pSrcSurface->LockRect(&sl, &sr, D3DLOCK_READONLY)))
		return D3DERR_INVALIDCALL;
	if (FAILED(pDestSurface->LockRect(&dl, &dr, 0)))
	{
		pSrcSurface->UnlockRect();
		return D3DERR_INVALIDCALL;
	}

	if (compressed)
	{
		// Same format and size: copy rows of 4x4 blocks.
		const UINT rowBytes = ((dw + 3) / 4) * BlockBytes(dd.Format);
		const UINT rows = (dh + 3) / 4;
		for (UINT y = 0; y < rows; ++y)
			memcpy((BYTE*)dl.pBits + y * dl.Pitch, (const BYTE*)sl.pBits + y * sl.Pitch, rowBytes);
	}
	else if (sameShape && ColorKey == 0)
	{
		for (UINT y = 0; y < dh; ++y)
			memcpy((BYTE*)dl.pBits + y * dl.Pitch, (const BYTE*)sl.pBits + y * sl.Pitch, dw * dbpp);
	}
	else
	{
		const DWORD filter = Filter & 0xFF;
		const bool average = (filter == D3DX_FILTER_BOX || filter == D3DX_FILTER_TRIANGLE || filter == D3DX_FILTER_LINEAR)
			&& (sw > dw || sh > dh);

		for (UINT y = 0; y < dh; ++y)
		{
			BYTE* drow = (BYTE*)dl.pBits + y * dl.Pitch;
			const UINT y0 = UINT((unsigned long long)y * sh / dh);
			const UINT y1 = average ? UINT((unsigned long long)(y + 1) * sh / dh) : y0 + 1;
			for (UINT x = 0; x < dw; ++x)
			{
				const UINT x0 = UINT((unsigned long long)x * sw / dw);
				const UINT x1 = average ? UINT((unsigned long long)(x + 1) * sw / dw) : x0 + 1;

				Color sum = { 0.0f, 0.0f, 0.0f, 0.0f };
				UINT count = 0;
				for (UINT sy = y0; sy < y1 && sy < sh; ++sy)
				{
					const BYTE* srow = (const BYTE*)sl.pBits + sy * sl.Pitch;
					for (UINT sx = x0; sx < x1 && sx < sw; ++sx)
					{
						Color c = Decode(sd.Format, ReadRaw(srow + sx * sbpp, sbpp));
						if (ColorKey != 0)
						{
							const DWORD argb = Encode(D3DFMT_A8R8G8B8, c);
							if (argb == ColorKey)
								c.a = c.r = c.g = c.b = 0.0f;
						}
						sum.a += c.a; sum.r += c.r; sum.g += c.g; sum.b += c.b;
						++count;
					}
				}
				if (count > 1)
				{
					const float inv = 1.0f / float(count);
					sum.a *= inv; sum.r *= inv; sum.g *= inv; sum.b *= inv;
				}
				WriteRaw(drow + x * dbpp, dbpp, Encode(dd.Format, sum));
			}
		}
	}

	pDestSurface->UnlockRect();
	pSrcSurface->UnlockRect();
	return D3D_OK;
}

static HRESULT FilterSurfaceChain(IDirect3DTexture8* tex, UINT srcLevel, DWORD filter)
{
	const UINT levels = tex->GetLevelCount();
	for (UINT level = srcLevel + 1; level < levels; ++level)
	{
		IDirect3DSurface8* src = nullptr;
		IDirect3DSurface8* dst = nullptr;
		HRESULT hr = tex->GetSurfaceLevel(level - 1, &src);
		if (SUCCEEDED(hr))
			hr = tex->GetSurfaceLevel(level, &dst);
		if (SUCCEEDED(hr))
			hr = D3DXLoadSurfaceFromSurface(dst, nullptr, nullptr, src, nullptr, nullptr, filter, 0);
		if (dst) dst->Release();
		if (src) src->Release();
		if (FAILED(hr))
			return hr;
	}
	return D3D_OK;
}

static HRESULT FilterCubeChain(IDirect3DCubeTexture8* tex, UINT srcLevel, DWORD filter)
{
	const UINT levels = tex->GetLevelCount();
	for (int face = 0; face < 6; ++face)
	{
		for (UINT level = srcLevel + 1; level < levels; ++level)
		{
			IDirect3DSurface8* src = nullptr;
			IDirect3DSurface8* dst = nullptr;
			HRESULT hr = tex->GetCubeMapSurface(D3DCUBEMAP_FACES(face), level - 1, &src);
			if (SUCCEEDED(hr))
				hr = tex->GetCubeMapSurface(D3DCUBEMAP_FACES(face), level, &dst);
			if (SUCCEEDED(hr))
				hr = D3DXLoadSurfaceFromSurface(dst, nullptr, nullptr, src, nullptr, nullptr, filter, 0);
			if (dst) dst->Release();
			if (src) src->Release();
			if (FAILED(hr))
				return hr;
		}
	}
	return D3D_OK;
}

extern "C" HRESULT WINAPI D3DXFilterTexture(LPDIRECT3DBASETEXTURE8 pBaseTexture, CONST PALETTEENTRY* pPalette,
	UINT SrcLevel, DWORD Filter)
{
	(void)pPalette;
	if (pBaseTexture == nullptr)
		return D3DERR_INVALIDCALL;

	const DWORD filter = (Filter == D3DX_DEFAULT) ? D3DX_FILTER_BOX : Filter;
	const UINT srcLevel = (SrcLevel == D3DX_DEFAULT) ? 0 : SrcLevel;

	switch (pBaseTexture->GetType())
	{
	case D3DRTYPE_TEXTURE:
		return FilterSurfaceChain(static_cast<IDirect3DTexture8*>(pBaseTexture), srcLevel, filter);
	case D3DRTYPE_CUBETEXTURE:
		return FilterCubeChain(static_cast<IDirect3DCubeTexture8*>(pBaseTexture), srcLevel, filter);
	default:
		// Volume mip filtering is not used by the engine.
		return D3D_OK;
	}
}

// ---------------------------------------------------------------------------
// Shader assembler
// ---------------------------------------------------------------------------

// D3DX9's ID3DXBuffer has the same vtable layout as D3DX8's (IUnknown,
// GetBufferPointer, GetBufferSize), and shader model 1.x token streams use the
// same encoding in D3D8 and D3D9.
typedef HRESULT (WINAPI *D3DXAssembleShader9Fn)(LPCSTR pSrcData, UINT SrcDataLen, const void* pDefines,
	void* pInclude, DWORD Flags, LPD3DXBUFFER* ppShader, LPD3DXBUFFER* ppErrorMsgs);

static D3DXAssembleShader9Fn GetAssembler()
{
	static bool searched = false;
	static D3DXAssembleShader9Fn fn = nullptr;
	if (!searched)
	{
		searched = true;
		static const char* const dlls[] = { "d3dx9_43.dll", "d3dx9_42.dll", "d3dx9_41.dll", "d3dx9_40.dll" };
		for (const char* name : dlls)
		{
			HMODULE module = LoadLibraryA(name);
			if (module == nullptr)
				continue;
			fn = (D3DXAssembleShader9Fn)(void*)GetProcAddress(module, "D3DXAssembleShader");
			if (fn != nullptr)
				break;
			FreeLibrary(module);
		}
	}
	return fn;
}

extern "C" HRESULT WINAPI D3DXAssembleShader(LPCVOID pSrcData, UINT SrcDataLen, DWORD Flags,
	LPD3DXBUFFER* ppConstants, LPD3DXBUFFER* ppCompiledShader, LPD3DXBUFFER* ppCompilationErrors)
{
	if (ppConstants)
		*ppConstants = nullptr;
	if (ppCompiledShader)
		*ppCompiledShader = nullptr;
	if (ppCompilationErrors)
		*ppCompilationErrors = nullptr;

	D3DXAssembleShader9Fn assemble = GetAssembler();
	if (assemble == nullptr)
	{
		// Callers treat failure as "shader not available" and use their fixed-function path.
		return D3DERR_NOTAVAILABLE;
	}
	return assemble((LPCSTR)pSrcData, SrcDataLen, nullptr, nullptr, Flags, ppCompiledShader, ppCompilationErrors);
}
