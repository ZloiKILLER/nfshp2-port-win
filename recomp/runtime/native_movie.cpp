// The movie decoder out of the emulation: DCTImage::ToImage and all it calls as plain C++ over the game's memory.
//
// The first module of the game moved to native code, and the pattern for the next ones: src/DCTImage.cpp and src/DCTBlock.cpp nearly as they
// are, on the game's own structures, where a pointer is a 32-bit guest address (G<T>). Huffman has no C++ in src/ and is written from retail's code.
// What is left out: the timings in dctTimeCurrent and friends, and DecompressionType 0 and 1 (retail's data says 2; anything else goes to the
// generated code). MoviePlayer, the ring buffer and DCTImage::Init stay generated.
//
// HP2_MOVIECHECK=1 runs this and the generated code on the same frame and compares all they write. HP2_MOVIEALL=1 does that for every frame of
// every movie on the disc at the first frame the game decodes, and exits. Built with -fwrapv: the game's arithmetic wraps.
#include "disc.h"
#include "ps2.h"

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

typedef signed char s8;

template <typename T> struct G {
    u32 a;
    T *p() const { return (T *)host(a); }
};

struct MotionVect {
    s16 vx, vy;
};

struct DCTImage {
    G<u8> pData, pY, pU, pV, pRowCount, pTransp, pDCTType;
    G<MotionVect> pVects;
    G<u8> pCompData;
    int CompSize;
    G<u8> pPrevData, pPrevY, pPrevU, pPrevV;
    G<MotionVect> pPrevVects;
    G<s16> pDCTData;
    int DCTDataSize;
    int XSize, YSize, Area, NumVects, NumBlocks, Format;
    s16 DLTab[2][64];
    s16 DCTab[2][64];
};
static_assert(sizeof(G<u8>) == 4 && offsetof(DCTImage, pDCTData) == 60 && offsetof(DCTImage, XSize) == 68 && offsetof(DCTImage, DLTab) == 92 && sizeof(DCTImage) == 604, "retail's DCTImage");

// the game's data, by retail address
enum : u32 {
    A_LUMA_QUANT = 0x2bdb30,
    A_CHROMA_QUANT = 0x2bdbb0,
    A_HUFF_PREFIX = 0x2bdc48,
    A_CLAMP = 0x331c90,
    A_OFFSET_CLAMP = 0x331c98,
    A_DECOMPRESSION_TYPE = 0x331cb4,
    A_DECODE_PATTERN = 0x334a68,
    A_DCT_TEMP = 0x334b68,
    A_HUFF_DECODE_TAB = 0x334be8,
};

static const int DCTBlockAdvance[4] = {0, 1, 4, 64};

static inline void put32(u8 *p, u32 v) { memcpy(p, &v, 4); }

// ---- Huffman: retail's huffman2.obj ---------------------------------------------------

struct Huffman {
    u32 accum = 0, accumCount = 0;
    const u8 *dsrc;

    explicit Huffman(const u8 *src) : dsrc(src) {}

    inline void fill()
    {
        while (accumCount < 25) {
            accum |= (u32)*dsrc++ << (24 - accumCount);
            accumCount += 8;
        }
    }

    u32 GetBits(u32 n)
    {
        fill();
        u32 v = accum >> ((0 - n) & 31);
        accum <<= n & 31;
        accumCount -= n;
        return v;
    }

    // Short codes come straight from a table of 512 pairs {length, value}. Value 0 is a run of zeros, -128 a longer code in length's class.
    void Decompress(s16 *out, int count)
    {
        const u8 *tab = g_ram + A_HUFF_DECODE_TAB;
        const u8 *prefix = g_ram + A_HUFF_PREFIX;
        s16 *end = out + count;
        while (out < end) {
            fill();
            u32 a = accum, n = accumCount, idx = a >> 23;
            int value = (s8)tab[idx * 2 + 1];
            u32 len = tab[idx * 2];
            if (value == -128) {
                u16 code_len;
                memcpy(&code_len, prefix + (len + 1) * 4 + 2, 2);
                a <<= code_len & 31;
                n -= code_len;
                u32 negative = a >> 31;
                a <<= 1;
                u32 v = (a >> ((0 - len) & 31)) + (1u << (len & 31));
                a <<= len & 31;
                accum = a;
                accumCount = n - 1 - len;
                *out++ = (s16)(negative ? 0 - v : v);
            } else if (value != 0) {
                *out++ = (s16)value;
                accum = a << (len & 31);
                accumCount = n - len;
            } else {
                a <<= 2;
                u32 is_long = a >> 31;
                a <<= 1;
                u32 run;
                if (is_long) {
                    run = (a >> 26) + 9;
                    accum = a << 6;
                    accumCount = n - 9;
                } else {
                    run = (a >> 29) + 1;
                    accum = a << 3;
                    accumCount = n - 6;
                }
                for (u32 i = 0; i < run; i++) *out++ = 0;
            }
        }
    }
};

// ---- the inverse transforms: src/DCTBlock.cpp, DecompressionType 2 ---------------------

#define MULTIPLY(x, c) (((x) * (c)) >> 8)

// the first pass, over the columns
static inline void idct_columns(const s16 *inptr, const s16 *quantptr, int *wsptr)
{
    for (int ctr = 8; ctr > 0; ctr--) {
        int tmp0 = inptr[0] * quantptr[0];
        int tmp1 = inptr[16] * quantptr[16];
        int tmp2 = inptr[32] * quantptr[32];
        int tmp3 = inptr[48] * quantptr[48];
        int tmp10 = tmp0 + tmp2;
        int tmp11 = tmp0 - tmp2;
        int tmp13 = tmp1 + tmp3;
        int tmp12 = MULTIPLY(tmp1 - tmp3, 362) - tmp13;
        tmp0 = tmp10 + tmp13;
        tmp3 = tmp10 - tmp13;
        tmp1 = tmp11 + tmp12;
        tmp2 = tmp11 - tmp12;
        int tmp4 = inptr[8] * quantptr[8];
        int tmp5 = inptr[24] * quantptr[24];
        int tmp6 = inptr[40] * quantptr[40];
        int tmp7 = inptr[56] * quantptr[56];
        int z13 = tmp6 + tmp5;
        int z10 = tmp6 - tmp5;
        int z11 = tmp4 + tmp7;
        int z12 = tmp4 - tmp7;
        tmp7 = z11 + z13;
        tmp11 = MULTIPLY(z11 - z13, 362);
        int z5 = MULTIPLY(z10 + z12, 473);
        tmp10 = MULTIPLY(z12, 277) - z5;
        tmp12 = MULTIPLY(z10, -669) + z5;
        tmp6 = tmp12 - tmp7;
        tmp5 = tmp11 - tmp6;
        tmp4 = tmp10 + tmp5;
        wsptr[0] = tmp0 + tmp7;
        wsptr[56] = tmp0 - tmp7;
        wsptr[8] = tmp1 + tmp6;
        wsptr[48] = tmp1 - tmp6;
        wsptr[16] = tmp2 + tmp5;
        wsptr[40] = tmp2 - tmp5;
        wsptr[32] = tmp3 + tmp4;
        wsptr[24] = tmp3 - tmp4;
        inptr++;
        quantptr++;
        wsptr++;
    }
}

// the second pass, over one row: the eight values before the shift
static inline void idct_row(const int *wsptr, int out[8])
{
    int tmp10 = wsptr[0] + wsptr[4];
    int tmp11 = wsptr[0] - wsptr[4];
    int tmp13 = wsptr[2] + wsptr[6];
    int tmp12 = MULTIPLY(wsptr[2] - wsptr[6], 362) - tmp13;
    int tmp0 = tmp10 + tmp13;
    int tmp3 = tmp10 - tmp13;
    int tmp1 = tmp11 + tmp12;
    int tmp2 = tmp11 - tmp12;
    int z13 = wsptr[5] + wsptr[3];
    int z10 = wsptr[5] - wsptr[3];
    int z11 = wsptr[1] + wsptr[7];
    int z12 = wsptr[1] - wsptr[7];
    int tmp7 = z11 + z13;
    tmp11 = MULTIPLY(z11 - z13, 362);
    int z5 = MULTIPLY(z10 + z12, 473);
    tmp10 = MULTIPLY(z12, 277) - z5;
    tmp12 = MULTIPLY(z10, -669) + z5;
    int tmp6 = tmp12 - tmp7;
    int tmp5 = tmp11 - tmp6;
    int tmp4 = tmp10 + tmp5;
    out[0] = tmp0 + tmp7;
    out[1] = tmp1 + tmp6;
    out[2] = tmp2 + tmp5;
    out[3] = tmp3 - tmp4;
    out[4] = tmp3 + tmp4;
    out[5] = tmp2 - tmp5;
    out[6] = tmp1 - tmp6;
    out[7] = tmp0 - tmp7;
}

struct Clamps {
    const u8 *clamp, *range;
};

static void IDCT(const Clamps &k, const s16 *pData, const s16 *pQuantTab, u8 *outptr, int Width)
{
    int workspace[64];
    idct_columns(pData, pQuantTab, workspace);
    const int *wsptr = workspace;
    for (int ctr = 0; ctr < 8; ctr++) {
        int v[8];
        idct_row(wsptr, v);
        for (int i = 0; i < 8; i++) outptr[i] = k.range[v[i] >> 5];
        wsptr += 8;
        outptr += Width;
    }
}

static void IDCTAdd(const Clamps &k, const s16 *pData, const s16 *pQuantTab, u8 *outptr, int Width)
{
    int workspace[64];
    idct_columns(pData, pQuantTab, workspace);
    const int *wsptr = workspace;
    for (int ctr = 0; ctr < 8; ctr++) {
        int v[8];
        idct_row(wsptr, v);
        for (int i = 0; i < 8; i++) outptr[i] = k.clamp[(v[i] >> 5) + outptr[i]];
        wsptr += 8;
        outptr += Width;
    }
}

static void IDCT1(const Clamps &k, const s16 *pData, const s16 *pQuantTab, u8 *pOutput, int Width)
{
    u8 dcval = k.range[(pData[0] * pQuantTab[0]) >> 5];
    for (int y = 0; y < 8; y++) {
        memset(pOutput, dcval, 8);
        pOutput += Width;
    }
}

static void IDCTAdd1(const Clamps &k, const s16 *pData, const s16 *pQuantTab, u8 *pOutput, int Width)
{
    int dcval = (pData[0] * pQuantTab[0]) >> 5;
    for (int y = 0; y < 8; y++) {
        for (int i = 0; i < 8; i++) pOutput[i] = k.clamp[dcval + pOutput[i]];
        pOutput += Width;
    }
}

// the first pass of the four-coefficient blocks: two columns
static inline void idct4_columns(const s16 *inptr, const s16 *quantptr, int *wsptr)
{
    for (int ctr = 2; ctr > 0; ctr--) {
        if (inptr[2] == 0) {
            int dcval = inptr[0] * quantptr[0];
            wsptr[0] = dcval;
            wsptr[8] = dcval;
            wsptr[16] = dcval;
            wsptr[24] = dcval;
            wsptr[32] = dcval;
            wsptr[40] = dcval;
            wsptr[48] = dcval;
            wsptr[56] = dcval;
        } else {
            int tmp4 = inptr[2] * quantptr[8];
            int tmp0 = inptr[0] * quantptr[0];
            int tmp11 = MULTIPLY(tmp4, 362);
            int z5 = MULTIPLY(tmp4, 473);
            int tmp10 = MULTIPLY(tmp4, 277) - z5;
            wsptr[56] = tmp0 - tmp4;
            wsptr[0] = tmp0 + tmp4;
            int tmp6 = z5 - tmp4;
            int tmp5 = tmp11 - tmp6;
            int tmp7 = tmp10 + tmp5;
            wsptr[8] = tmp0 + tmp6;
            wsptr[48] = tmp0 - tmp6;
            wsptr[16] = tmp0 + tmp5;
            wsptr[40] = tmp0 - tmp5;
            wsptr[32] = tmp0 + tmp7;
            wsptr[24] = tmp0 - tmp7;
        }
        inptr++;
        quantptr++;
        wsptr++;
    }
}

// the second pass of those: z11 not 0
static inline void idct4_row(int tmp0, int z11, int v[8])
{
    int tmp11 = MULTIPLY(z11, 362);
    int z5 = MULTIPLY(z11, 473);
    int tmp10 = MULTIPLY(z11, 277) - z5;
    int tmp6 = z5 - z11;
    int tmp5 = tmp11 - tmp6;
    int tmp4 = tmp10 + tmp5;
    v[0] = tmp0 + z11;
    v[1] = tmp0 + tmp6;
    v[2] = tmp0 + tmp5;
    v[3] = tmp0 - tmp4;
    v[4] = tmp0 + tmp4;
    v[5] = tmp0 - tmp5;
    v[6] = tmp0 - tmp6;
    v[7] = tmp0 - z11;
}

static void IDCT4(const Clamps &k, const s16 *pData, const s16 *pQuantTab, u8 *outptr, int Width)
{
    int workspace[64];
    idct4_columns(pData, pQuantTab, workspace);
    const int *wsptr = workspace;
    for (int ctr = 0; ctr < 8; ctr++) {
        int z11 = wsptr[1], tmp0 = wsptr[0];
        if (z11 == 0) {
            memset(outptr, k.range[tmp0 >> 5], 8);
        } else {
            int v[8];
            idct4_row(tmp0, z11, v);
            for (int i = 0; i < 8; i++) outptr[i] = k.range[v[i] >> 5];
        }
        wsptr += 8;
        outptr += Width;
    }
}

static void IDCTAdd4(const Clamps &k, const s16 *pData, const s16 *pQuantTab, u8 *outptr, int Width)
{
    int workspace[64];
    idct4_columns(pData, pQuantTab, workspace);
    const int *wsptr = workspace;
    for (int ctr = 0; ctr < 8; ctr++) {
        int z11 = wsptr[1], tmp0 = wsptr[0];
        if (z11 == 0) {
            int Val = tmp0 >> 5;
            for (int i = 0; i < 8; i++) outptr[i] = k.clamp[Val + outptr[i]];
        } else {
            int v[8];
            idct4_row(tmp0, z11, v);
            for (int i = 0; i < 8; i++) outptr[i] = k.clamp[(v[i] >> 5) + outptr[i]];
        }
        wsptr += 8;
        outptr += Width;
    }
}

// ---- the frame: src/DCTImage.cpp ------------------------------------------------------

static void ScaleTable(int Scale, s16 *pTable)
{
    for (int i = 0; i < 64; i++) {
        int Val = (pTable[i] * Scale + 50) / 100;
        if (Val <= 0) Val = 1;
        else if (Val > 32767) Val = 32767;
        pTable[i] = (s16)Val;
    }
}

static int ConvertQualityToScale(int quality)
{
    if (quality <= 0) quality = 1;
    if (quality > 100) quality = 100;
    return quality < 50 ? 5000 / quality : 200 - quality * 2;
}

static void GenerateTable(u32 base, int Quality, s16 *pDest)
{
    memcpy(pDest, g_ram + base, 128);
    ScaleTable(ConvertQualityToScale(Quality), pDest);
}

static void InitIDCTTables(DCTImage &d, s8 Qual, s8 DeltQual)
{
    static const s16 aanscales[64] = {16384, 22725, 21407, 19266, 16384, 12873, 8867, 4520, 22725, 31521, 29692, 26722, 22725, 17855, 12299, 6270, 21407, 29692, 27969, 25172, 21407, 16819, 11585, 5906, 19266, 26722, 25172, 22654, 19266, 15137, 10426, 5315, 16384, 22725, 21407, 19266, 16384, 12873, 8867, 4520, 12873, 17855, 16819, 15137, 12873, 10114, 6967, 3552, 8867, 12299, 11585, 10426, 8867, 6967, 4799, 2446, 4520, 6270, 5906, 5315, 4520, 3552, 2446, 1247};
    int Qual2 = Qual + DeltQual;
    GenerateTable(A_LUMA_QUANT, Qual, d.DLTab[0]);
    GenerateTable(A_LUMA_QUANT, Qual2, d.DLTab[1]);
    GenerateTable(A_CHROMA_QUANT, Qual, d.DCTab[0]);
    GenerateTable(A_CHROMA_QUANT, Qual2, d.DCTab[1]);
    for (int i = 0; i < 64; i++) {
        d.DLTab[0][i] = (s16)((d.DLTab[0][i] * aanscales[i] + 2048) >> 12);
        d.DLTab[1][i] = (s16)((d.DLTab[1][i] * aanscales[i] + 2048) >> 12);
        d.DCTab[0][i] = (s16)((d.DCTab[0][i] * aanscales[i] + 2048) >> 12);
        d.DCTab[1][i] = (s16)((d.DCTab[1][i] * aanscales[i] + 2048) >> 12);
    }
}

// the coefficients in zigzag order into DCTTemp, which is the game's
static inline const s16 *unzigzag(const s16 *pData)
{
    s16 *temp = (s16 *)(g_ram + A_DCT_TEMP);
    const int *pattern = (const int *)(g_ram + A_DECODE_PATTERN);
    for (int i = 0; i < 64; i++) temp[pattern[i]] = pData[i];
    return temp;
}

static void ToImage32(const DCTImage &d, const Clamps &k, u8 *pDestBuffer, int Flags)
{
    const int XSize = d.XSize, YSize = d.YSize;
    const u8 *pU = d.pU.p(), *pV = d.pV.p(), *pYDat = d.pY.p();
    const u8 *Clamp = k.clamp;
    int XRowInc;
    u8 *pDestRow;
    if (Flags & 1) {
        pDestRow = pDestBuffer + XSize * 4 * (YSize - 1);
        XRowInc = -XSize * 4;
    } else {
        pDestRow = pDestBuffer;
        XRowInc = XSize * 4;
    }
    int j = 0;
    for (int y = 0; y < YSize; y += 2) {
        u8 *pDest = pDestRow;
        for (int x = 0; x < XSize; x += 2) {
            int V = pV[j] - 128;
            int U = pU[j] - 128;
            int uv1 = (V * 359) / 256;
            int uv2 = (U * -88 - V * 183) / 256;
            int uv3 = (U * 454) / 256;
            const int luma[4] = {pYDat[0], pYDat[1], pYDat[XSize], pYDat[XSize + 1]};
            u8 *const out[4] = {pDest, pDest + 4, pDest + XRowInc, pDest + XRowInc + 4};
            for (int i = 0; i < 4; i++) {
                out[i][0] = Clamp[luma[i] + uv1];
                out[i][1] = Clamp[luma[i] + uv2];
                out[i][2] = Clamp[luma[i] + uv3];
                out[i][3] = 255;
            }
            pDest += 8;
            pYDat += 2;
            j++;
        }
        pYDat += XSize;
        pDestRow += XRowInc * 2;
    }
}

static inline void CopyPreviousFrameBlock(const DCTImage &d, int x, int y, u8 *pYDest, u8 *pUDest, u8 *pVDest, int VectIndex)
{
    const MotionVect *pMot = d.pVects.p() + VectIndex;
    const int XSize = d.XSize, HalfX = XSize / 2;
    const int vx = x + pMot->vx, vy = y + pMot->vy;
    const u8 *src = d.pPrevY.p() + vy * XSize + vx;
    for (int i = 0; i < 16; i++) memcpy(pYDest + i * XSize, src + i * XSize, 16);
    const int Offset = (vy / 2) * HalfX + vx / 2;
    const u8 *srcu = d.pPrevU.p() + Offset, *srcv = d.pPrevV.p() + Offset;
    for (int i = 0; i < 8; i++) memcpy(pUDest + i * HalfX, srcu + i * HalfX, 8);
    for (int i = 0; i < 8; i++) memcpy(pVDest + i * HalfX, srcv + i * HalfX, 8);
}

static void ToImage(DCTImage &d, u32 dest, int Qual, s8 DeltQual, int Flags)
{
    const Clamps k = {host(LW(A_CLAMP)), host(LW(A_OFFSET_CLAMP))};
    const u8 *pCompData = d.pCompData.p();
    const int XSize = d.XSize, YSize = d.YSize, HalfX = XSize / 2;
    int TIndex = 0, VectSize = 0, ReserveHeader, DCTTypeSize;
    s16 s;
    InitIDCTTables(d, (s8)Qual, DeltQual);
    const bool bIsKeyframe = Flags & 2;
    u8 *pTransp = d.pTransp.p();
    memcpy(&s, pCompData, 2);
    DCTTypeSize = s;
    if (bIsKeyframe) {
        ReserveHeader = 2;
    } else {
        ReserveHeader = (d.NumVects + 3) / 4 + 4;
        memcpy(&s, pCompData + 2, 2);
        VectSize = s;
        memcpy(pTransp, pCompData + 4, (d.NumVects + 3) / 4);
    }
    d.pDCTType.a = d.pCompData.a + ReserveHeader;
    const u8 *pDCTType = pCompData + ReserveHeader;
    u8 *pYDest = d.pY.p(), *pUDest = d.pU.p(), *pVDest = d.pV.p();
    s16 *const pDCTData = d.pDCTData.p();
    Huffman Huff(pDCTType + DCTTypeSize);
    int RowCount = XSize / 16;
    if (!bIsKeyframe) Huff.Decompress((s16 *)d.pVects.p(), VectSize * 2);
    int BlockIndex = 0, VectIndex = 0;

#define BLOCK_TYPE(n) ((pDCTType[(n) / 4] >> (((n) & 3) * 2)) & 3)
#define ADD_BLOCK(dst, width, quant)                                        \
    DCTType = BLOCK_TYPE(BlockIndex);                                       \
    switch (DCTType) {                                                      \
    case 1: IDCTAdd1(k, pDestPtr, quant, dst, width); break;                \
    case 2: IDCTAdd4(k, pDestPtr, quant, dst, width); break;                \
    case 3: IDCTAdd(k, unzigzag(pDestPtr), quant, dst, width); break;       \
    }                                                                       \
    BlockIndex++;                                                           \
    pDestPtr += DCTBlockAdvance[DCTType];
#define INTRA_BLOCK(dst, width, quant)                                      \
    DCTType = BLOCK_TYPE(BlockIndex);                                       \
    switch (DCTType) {                                                      \
    case 0:                                                                 \
        for (int yy = 0; yy < 8; yy++) memset(dst + yy * (width), 0x80, 8); \
        break;                                                              \
    case 1: IDCT1(k, pDestPtr, quant, dst, width); break;                   \
    case 2: IDCT4(k, pDestPtr, quant, dst, width); break;                   \
    case 3: IDCT(k, unzigzag(pDestPtr), quant, dst, width); break;          \
    }                                                                       \
    BlockIndex++;                                                           \
    pDestPtr += DCTBlockAdvance[DCTType];

    for (int y = 0; y < YSize; y += 16) {
        int RowSize = Huff.GetBits(16);
        if (!bIsKeyframe) RowCount = Huff.GetBits(8);
        Huff.Decompress(pDCTData, RowSize);
        // the first coefficient of a block is the difference from the block before it
        int YPrev = 0, UPrev = 0, VPrev = 0;
        s16 *pDestPtr = pDCTData;
        int Index = BlockIndex, DCTType;
        for (int i = 0; i < RowCount; i++) {
            for (int b = 0; b < 4; b++) {
                DCTType = BLOCK_TYPE(Index);
                if (DCTBlockAdvance[DCTType]) {
                    YPrev = (s16)(*pDestPtr += (s16)YPrev);
                    pDestPtr += DCTBlockAdvance[DCTType];
                }
                Index++;
            }
            DCTType = BLOCK_TYPE(Index);
            if (DCTBlockAdvance[DCTType]) {
                UPrev = (s16)(*pDestPtr += (s16)UPrev);
                pDestPtr += DCTBlockAdvance[DCTType];
            }
            Index++;
            DCTType = BLOCK_TYPE(Index);
            if (DCTBlockAdvance[DCTType]) {
                VPrev = (s16)(*pDestPtr += (s16)VPrev);
                pDestPtr += DCTBlockAdvance[DCTType];
            }
            Index++;
        }
        pDestPtr = pDCTData;
        for (int x = 0; x < XSize; x += 16) {
            if (!bIsKeyframe && ((pTransp[TIndex >> 2] >> (TIndex & 3)) & 1)) {
                CopyPreviousFrameBlock(d, x, y, pYDest, pUDest, pVDest, VectIndex);
                if (!(pTransp[TIndex >> 2] & (16 << (TIndex & 3)))) {
                    ADD_BLOCK(pYDest, XSize, d.DLTab[1])
                    ADD_BLOCK((pYDest + 8), XSize, d.DLTab[1])
                    ADD_BLOCK((pYDest + XSize * 8), XSize, d.DLTab[1])
                    ADD_BLOCK((pYDest + XSize * 8 + 8), XSize, d.DLTab[1])
                    ADD_BLOCK(pUDest, HalfX, d.DCTab[1])
                    ADD_BLOCK(pVDest, HalfX, d.DCTab[1])
                }
                VectIndex++;
            } else {
                INTRA_BLOCK(pYDest, XSize, d.DLTab[0])
                INTRA_BLOCK((pYDest + 8), XSize, d.DLTab[0])
                INTRA_BLOCK((pYDest + XSize * 8), XSize, d.DLTab[0])
                INTRA_BLOCK((pYDest + XSize * 8 + 8), XSize, d.DLTab[0])
                INTRA_BLOCK(pUDest, HalfX, d.DCTab[0])
                INTRA_BLOCK(pVDest, HalfX, d.DCTab[0])
            }
            pYDest += 16;
            pUDest += 8;
            pVDest += 8;
            TIndex++;
        }
        pYDest += XSize * 15;
        pUDest += HalfX * 7;
        pVDest += HalfX * 7;
    }
    if ((Flags & 4) && (d.Format == 0 || d.Format == 1)) ToImage32(d, k, host(dest), Flags);
    // the frame just made becomes the previous one
    std::swap(d.pData, d.pPrevData);
    std::swap(d.pY, d.pPrevY);
    std::swap(d.pU, d.pPrevU);
    std::swap(d.pV, d.pPrevV);
    std::swap(d.pVects, d.pPrevVects);
}

// ---- both ways, compared --------------------------------------------------------------

static u64 g_frames, g_differ;
static double g_native_ms, g_generated_ms;
static const char *g_what = "";
static int g_frame_no;

struct Region {
    const char *name;
    u8 *p;
    size_t n;
    std::vector<u8> before, mine;
};

static double ms_since(std::chrono::steady_clock::time_point t)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

// everything ToImage writes, but for the timings: first this code from a copy of the state, then the generated code from the same state
static void checked(Ctx &c, void (*orig)(Ctx &), DCTImage &d, u32 dest, int Qual, s8 DeltQual, int Flags)
{
    const int ysize = (d.Area + 127) & ~127, uvsize = (ysize + 127) & ~127;
    const size_t total = ysize + ((d.XSize / 16) * (d.YSize / 16)) * 4 + uvsize * 2;
    Region r[5] = {
        {"struct", (u8 *)&d, sizeof d},
        {"frame", d.pData.p(), total},
        {"transp", d.pTransp.p(), (size_t)(d.NumVects + 3) / 4},
        {"coefficients", (u8 *)d.pDCTData.p(), (size_t)d.DCTDataSize * 2},
        {"DCTTemp", g_ram + A_DCT_TEMP, 128},
    };
    Region image = {"image", host(dest), (Flags & 4) ? (size_t)d.XSize * d.YSize * 4 : 0};
    for (Region &a : r) a.before.assign(a.p, a.p + a.n);
    image.before.assign(image.p, image.p + image.n);

    auto t = std::chrono::steady_clock::now();
    ToImage(d, dest, Qual, DeltQual, Flags);
    g_native_ms += ms_since(t);
    for (Region &a : r) a.mine.assign(a.p, a.p + a.n);
    image.mine.assign(image.p, image.p + image.n);
    for (Region &a : r) memcpy(a.p, a.before.data(), a.n);
    memcpy(image.p, image.before.data(), image.n);

    t = std::chrono::steady_clock::now();
    orig(c);
    g_generated_ms += ms_since(t);

    g_frames++;
    bool bad = false;
    auto compare = [&](Region &a) {
        if (a.n && memcmp(a.p, a.mine.data(), a.n)) {
            size_t at = 0;
            while (a.p[at] == a.mine[at]) at++;
            bad = true;
            if (g_differ < 20)
                fprintf(stderr, "moviecheck: %s frame %d: %s differs at %zu: native %02x, generated %02x (flags %d)\n", g_what, g_frame_no, a.name, at, a.mine[at], a.p[at], Flags);
        }
    };
    for (Region &a : r) compare(a);
    compare(image);
    if (bad) g_differ++;
    if (g_frames % 3000 == 0)
        fprintf(stderr, "moviecheck: %llu frames, %llu differ, native %.2f ms, generated %.2f ms a frame\n", (unsigned long long)g_frames, (unsigned long long)g_differ, g_native_ms / g_frames, g_generated_ms / g_frames);
}

static u32 rd32(const u8 *p)
{
    u32 v;
    memcpy(&v, p, 4);
    return v;
}

static u32 guest_alloc(Ctx &c, u32 size)
{
    Ctx t = c;
    t.r[4].d[0] = size;
    t.r[5].d[0] = 64;
    ps2_call(t, 0x257690);      // bMalloc(int, int)
    return t.r[2].w[0];
}

// Every frame of every movie on the disc, on the DCTImage of the movie now playing. It ends the game.
static void check_all_movies(Ctx &c, void (*orig)(Ctx &), DCTImage &d)
{
    DiscFile zdir, zzdata;
    if (!disc_find("\\NFSHP2\\ZDIR.BIN", zdir) || !disc_find("\\NFSHP2\\ZZDATA.BIN", zzdata)) {
        fprintf(stderr, "moviecheck: no game data\n");
        std::_Exit(1);
    }
    std::vector<u8> dir(zdir.size);
    disc_read_file(zdir, 0, zdir.size, dir.data());
    const u32 frame_buf = guest_alloc(c, 0x80000), image_buf = guest_alloc(c, 512 * 384 * 4);
    fprintf(stderr, "moviecheck: buffers %08x %08x\n", frame_buf, image_buf);
    if (!frame_buf || !image_buf) std::_Exit(1);
    const u32 self = (u32)((u8 *)&d - g_ram);
    int movies = 0;
    for (size_t e = 0; e + 12 <= dir.size(); e += 12) {
        const u32 hash = rd32(&dir[e]), lba = rd32(&dir[e + 4]), size = rd32(&dir[e + 8]);
        u8 head[12];
        disc_read_file(zzdata, lba * 2048, 12, head);
        if (memcmp(head, "RIFF", 4) || memcmp(head + 8, "AVI ", 4)) continue;
        std::vector<u8> avi(size);
        disc_read_file(zzdata, lba * 2048, size, avi.data());
        const std::string text((const char *)avi.data(), 4096);
        size_t strf = text.find("strf"), movi = text.find("movi");
        if (strf == std::string::npos || movi == std::string::npos) continue;
        const u8 *bih = avi.data() + strf + 8;
        const int w = (int)rd32(bih + 4), h = (int)rd32(bih + 8);
        char name[64];
        snprintf(name, sizeof name, "%08x %dx%d", hash, w, h);
        g_what = name;
        if (memcmp(bih + 16, "MV30", 4)) {
            fprintf(stderr, "moviecheck: %s: not MV30, the game cannot play it\n", name);
            continue;
        }
        // DCTImage::Init(w, h, false, 1) in the generated code: it frees the old buffers
        Ctx t = c;
        t.r[4].d[0] = self;
        t.r[5].d[0] = (u32)w;
        t.r[6].d[0] = (u32)h;
        t.r[7].d[0] = 0;
        t.r[8].d[0] = 1;
        ps2_call(t, 0x234560);
        const u64 frames_before = g_frames, differ_before = g_differ;
        g_frame_no = 0;
        for (size_t p = movi + 4; p + 8 <= avi.size();) {
            const u32 len = rd32(&avi[p + 4]);
            // the frames end at the index; past it some files hold leftovers that only look like chunks
            if (!memcmp(&avi[p], "idx1", 4) || p + 8 + len > avi.size()) break;
            if (!memcmp(&avi[p], "00d", 3)) {
                if (len > 0x80000 || len < 4) {
                    fprintf(stderr, "moviecheck: %s frame %d: size %u skipped\n", name, g_frame_no, len);
                } else {
                    const u8 *f = &avi[p + 8];
                    memcpy(g_ram + frame_buf, f, len);
                    u16 inter;
                    memcpy(&inter, f + 2, 2);
                    // an RGB picture now and then, both ways up: the game's other output
                    int Flags = (inter == 0 ? 2 : 0) | (g_frame_no % 16 == 7 ? 4 : 0) | (g_frame_no % 32 == 7 ? 1 : 0);
                    d.pCompData.a = frame_buf + 4;
                    t = c;
                    t.r[4].d[0] = self;
                    t.r[5].d[0] = image_buf;
                    t.r[6].d[0] = (u64)(s64)(s8)f[0];
                    t.r[7].d[0] = (u64)(s64)(s8)f[1];
                    t.r[8].d[0] = (u32)Flags;
                    checked(t, orig, d, image_buf, (s8)f[0], (s8)f[1], Flags);
                }
                g_frame_no++;
            }
            p += 8 + len + (len & 1);
        }
        movies++;
        fprintf(stderr, "moviecheck: %s: %llu frames, %llu differ\n", name, (unsigned long long)(g_frames - frames_before), (unsigned long long)(g_differ - differ_before));
    }
    fprintf(stderr, "moviecheck: all %d movies: %llu frames, %llu differ, native %.3f ms, generated %.3f ms a frame\n", movies, (unsigned long long)g_frames, (unsigned long long)g_differ, g_native_ms / g_frames, g_generated_ms / g_frames);
    fflush(stderr);
    std::_Exit(g_differ ? 2 : 0);
}

void HOOK_ToImage__8DCTImagePUcici(Ctx &c, void (*orig)(Ctx &))
{
    if (g_irq_pending) ps2_take_interrupts(c);
    if (LW(A_DECOMPRESSION_TYPE) != 2) {
        orig(c);
        return;
    }
    static const bool check_all = getenv("HP2_MOVIEALL") != nullptr, check = getenv("HP2_MOVIECHECK") != nullptr;
    DCTImage &d = *(DCTImage *)host(c.r[4].w[0]);
    if (check_all) check_all_movies(c, orig, d);
    const u32 dest = c.r[5].w[0];
    const int Qual = (s32)c.r[6].w[0], Flags = (s32)c.r[8].w[0];
    const s8 DeltQual = (s8)c.r[7].w[0];
    if (check) {
        g_what = "game";
        g_frame_no++;
        checked(c, orig, d, dest, Qual, DeltQual, Flags);
        return;
    }
    ToImage(d, dest, Qual, DeltQual, Flags);
}
