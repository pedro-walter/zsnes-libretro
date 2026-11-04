/* MMX semantics for lifted code */
#ifndef LIFT_MMX_H
#define LIFT_MMX_H
#define MMX_LANES_B(OP) for (int i_ = 0; i_ < 8; i_++) { OP; }
static inline u8 satu8(int v) { return v < 0 ? 0 : v > 255 ? 255 : (u8)v; }
static inline s8 sats8(int v) { return v < -128 ? -128 : v > 127 ? 127 : (s8)v; }
static inline u16 satu16(int v) { return v < 0 ? 0 : v > 65535 ? 65535 : (u16)v; }
static inline s16 sats16(int v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : (s16)v; }
static inline mmx_t mm_paddb(mmx_t a, mmx_t b) { for (int i = 0; i < 8; i++) a.b[i] += b.b[i]; return a; }
static inline mmx_t mm_paddw(mmx_t a, mmx_t b) { for (int i = 0; i < 4; i++) a.w[i] += b.w[i]; return a; }
static inline mmx_t mm_paddd(mmx_t a, mmx_t b) { for (int i = 0; i < 2; i++) a.d[i] += b.d[i]; return a; }
static inline mmx_t mm_paddsb(mmx_t a, mmx_t b) { for (int i = 0; i < 8; i++) a.sb[i] = sats8(a.sb[i] + b.sb[i]); return a; }
static inline mmx_t mm_paddsw(mmx_t a, mmx_t b) { for (int i = 0; i < 4; i++) a.sw[i] = sats16(a.sw[i] + b.sw[i]); return a; }
static inline mmx_t mm_paddusb(mmx_t a, mmx_t b) { for (int i = 0; i < 8; i++) a.b[i] = satu8(a.b[i] + b.b[i]); return a; }
static inline mmx_t mm_paddusw(mmx_t a, mmx_t b) { for (int i = 0; i < 4; i++) a.w[i] = satu16(a.w[i] + b.w[i]); return a; }
static inline mmx_t mm_psubb(mmx_t a, mmx_t b) { for (int i = 0; i < 8; i++) a.b[i] -= b.b[i]; return a; }
static inline mmx_t mm_psubw(mmx_t a, mmx_t b) { for (int i = 0; i < 4; i++) a.w[i] -= b.w[i]; return a; }
static inline mmx_t mm_psubd(mmx_t a, mmx_t b) { for (int i = 0; i < 2; i++) a.d[i] -= b.d[i]; return a; }
static inline mmx_t mm_psubsb(mmx_t a, mmx_t b) { for (int i = 0; i < 8; i++) a.sb[i] = sats8(a.sb[i] - b.sb[i]); return a; }
static inline mmx_t mm_psubsw(mmx_t a, mmx_t b) { for (int i = 0; i < 4; i++) a.sw[i] = sats16(a.sw[i] - b.sw[i]); return a; }
static inline mmx_t mm_psubusb(mmx_t a, mmx_t b) { for (int i = 0; i < 8; i++) a.b[i] = satu8((int)a.b[i] - b.b[i]); return a; }
static inline mmx_t mm_psubusw(mmx_t a, mmx_t b) { for (int i = 0; i < 4; i++) a.w[i] = satu16((int)a.w[i] - b.w[i]); return a; }
static inline mmx_t mm_pmullw(mmx_t a, mmx_t b) { for (int i = 0; i < 4; i++) a.sw[i] = (s16)((s32)a.sw[i] * b.sw[i]); return a; }
static inline mmx_t mm_pmulhw(mmx_t a, mmx_t b) { for (int i = 0; i < 4; i++) a.sw[i] = (s16)(((s32)a.sw[i] * b.sw[i]) >> 16); return a; }
static inline mmx_t mm_pmaddwd(mmx_t a, mmx_t b) { mmx_t r; r.sd[0] = (s32)a.sw[0] * b.sw[0] + (s32)a.sw[1] * b.sw[1]; r.sd[1] = (s32)a.sw[2] * b.sw[2] + (s32)a.sw[3] * b.sw[3]; return r; }
static inline mmx_t mm_pand(mmx_t a, mmx_t b) { a.q &= b.q; return a; }
static inline mmx_t mm_pandn(mmx_t a, mmx_t b) { a.q = ~a.q & b.q; return a; }
static inline mmx_t mm_por(mmx_t a, mmx_t b) { a.q |= b.q; return a; }
static inline mmx_t mm_pxor(mmx_t a, mmx_t b) { a.q ^= b.q; return a; }
static inline mmx_t mm_pcmpeqb(mmx_t a, mmx_t b) { for (int i = 0; i < 8; i++) a.b[i] = a.b[i] == b.b[i] ? 0xFF : 0; return a; }
static inline mmx_t mm_pcmpeqw(mmx_t a, mmx_t b) { for (int i = 0; i < 4; i++) a.w[i] = a.w[i] == b.w[i] ? 0xFFFF : 0; return a; }
static inline mmx_t mm_pcmpeqd(mmx_t a, mmx_t b) { for (int i = 0; i < 2; i++) a.d[i] = a.d[i] == b.d[i] ? 0xFFFFFFFFu : 0; return a; }
static inline mmx_t mm_pcmpgtb(mmx_t a, mmx_t b) { for (int i = 0; i < 8; i++) a.b[i] = a.sb[i] > b.sb[i] ? 0xFF : 0; return a; }
static inline mmx_t mm_pcmpgtw(mmx_t a, mmx_t b) { for (int i = 0; i < 4; i++) a.w[i] = a.sw[i] > b.sw[i] ? 0xFFFF : 0; return a; }
static inline mmx_t mm_pcmpgtd(mmx_t a, mmx_t b) { for (int i = 0; i < 2; i++) a.d[i] = a.sd[i] > b.sd[i] ? 0xFFFFFFFFu : 0; return a; }
static inline mmx_t mm_packuswb(mmx_t a, mmx_t b) { mmx_t r; for (int i = 0; i < 4; i++) { r.b[i] = satu8(a.sw[i]); r.b[i + 4] = satu8(b.sw[i]); } return r; }
static inline mmx_t mm_packsswb(mmx_t a, mmx_t b) { mmx_t r; for (int i = 0; i < 4; i++) { r.sb[i] = sats8(a.sw[i]); r.sb[i + 4] = sats8(b.sw[i]); } return r; }
static inline mmx_t mm_packssdw(mmx_t a, mmx_t b) { mmx_t r; for (int i = 0; i < 2; i++) { r.sw[i] = sats16(a.sd[i]); r.sw[i + 2] = sats16(b.sd[i]); } return r; }
static inline mmx_t mm_punpcklbw(mmx_t a, mmx_t b) { mmx_t r; for (int i = 0; i < 4; i++) { r.b[2 * i] = a.b[i]; r.b[2 * i + 1] = b.b[i]; } return r; }
static inline mmx_t mm_punpcklwd(mmx_t a, mmx_t b) { mmx_t r; for (int i = 0; i < 2; i++) { r.w[2 * i] = a.w[i]; r.w[2 * i + 1] = b.w[i]; } return r; }
static inline mmx_t mm_punpckldq(mmx_t a, mmx_t b) { mmx_t r; r.d[0] = a.d[0]; r.d[1] = b.d[0]; return r; }
static inline mmx_t mm_punpckhbw(mmx_t a, mmx_t b) { mmx_t r; for (int i = 0; i < 4; i++) { r.b[2 * i] = a.b[i + 4]; r.b[2 * i + 1] = b.b[i + 4]; } return r; }
static inline mmx_t mm_punpckhwd(mmx_t a, mmx_t b) { mmx_t r; for (int i = 0; i < 2; i++) { r.w[2 * i] = a.w[i + 2]; r.w[2 * i + 1] = b.w[i + 2]; } return r; }
static inline mmx_t mm_punpckhdq(mmx_t a, mmx_t b) { mmx_t r; r.d[0] = a.d[1]; r.d[1] = b.d[1]; return r; }
static inline mmx_t mm_psllw(mmx_t a, u32 c) { for (int i = 0; i < 4; i++) a.w[i] = c > 15 ? 0 : (u16)(a.w[i] << c); return a; }
static inline mmx_t mm_pslld(mmx_t a, u32 c) { for (int i = 0; i < 2; i++) a.d[i] = c > 31 ? 0 : a.d[i] << c; return a; }
static inline mmx_t mm_psllq(mmx_t a, u32 c) { a.q = c > 63 ? 0 : a.q << c; return a; }
static inline mmx_t mm_psrlw(mmx_t a, u32 c) { for (int i = 0; i < 4; i++) a.w[i] = c > 15 ? 0 : (u16)(a.w[i] >> c); return a; }
static inline mmx_t mm_psrld(mmx_t a, u32 c) { for (int i = 0; i < 2; i++) a.d[i] = c > 31 ? 0 : a.d[i] >> c; return a; }
static inline mmx_t mm_psrlq(mmx_t a, u32 c) { a.q = c > 63 ? 0 : a.q >> c; return a; }
static inline mmx_t mm_psraw(mmx_t a, u32 c) { for (int i = 0; i < 4; i++) a.sw[i] = a.sw[i] >> (c > 15 ? 15 : c); return a; }
static inline mmx_t mm_psrad(mmx_t a, u32 c) { for (int i = 0; i < 2; i++) a.sd[i] = a.sd[i] >> (c > 31 ? 31 : c); return a; }
#define mm_ld(a) ({ mmx_t r_; r_.q = M64(a); r_; })
#endif
