#include <stdint.h>
#include "zlift.h"
/*
Copyright (C) 1997-2005 ZSNES Team ( zsKnight, _Demo_, pagefault, Nach )

http://www.zsnes.com
http://sourceforge.net/projects/zsnes

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either
version 2 of the License, or (at your option) any later
version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/




#ifdef __LINUX__
#include "gblhdr.h"
#define DIR_SLASH "/"
#else
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define DIR_SLASH "\\"
#endif

#define memcpyinc(dest, src, size) memcpy(dest, src, size); dest += size;
#define memcpyrinc(src, dest, size) memcpy(dest, src, size); src += size;


/*Let's start converting stuff from execute.asm ^_^;
Big thanks to Nach and TRAC for helping me out on porting !!*/

#define CBackupPos (*(unsigned int *)0x34033e74u)
#define PHnum2writecpureg (*(unsigned int *)0x340a4478u)
#define cycpbl (*(unsigned int *)0x340a4470u)
#define wramdata ((unsigned int *)(uintptr_t)(*(volatile unsigned int *)0x340a9f7cu))
#define wramdata_SLOT (*(volatile unsigned int *)0x340a9f7cu)
#define vram ((unsigned int *)(uintptr_t)(*(volatile unsigned int *)0x340a9f84u))
#define vram_SLOT (*(volatile unsigned int *)0x340a9f84u)
#define PHspcsave (*(unsigned int *)0x34047fc0u)
#define C4Ram ((unsigned int *)(uintptr_t)(*(volatile unsigned int *)0x340a7059u))
#define C4Ram_SLOT (*(volatile unsigned int *)0x340a7059u)
#define sfxramdata ((unsigned int *)(uintptr_t)(*(volatile unsigned int *)0x340a9f74u))
#define sfxramdata_SLOT (*(volatile unsigned int *)0x340a9f74u)
#define PHnum2writesa1reg (*(unsigned int *)0x340052f7u)
#define SA1Mode (*(unsigned int *)0x34004842u)
#define prevedi (*(unsigned int *)0x34004783u)
#define SA1xpc (*(unsigned int *)0x34004787u)
#define sa1dmaptr (*(unsigned int *)0x34015333u)
#define soundcycleft (*(unsigned int *)0x34034451u)
#define spc700read (*(unsigned int *)0x3403458du)
#define timer2upd (*(unsigned int *)0x3404800cu)
#define xa (*(unsigned int *)0x340a4490u)
#define spcnumread (*(unsigned int *)0x34048004u)
#define spchalted (*(unsigned int *)0x34048008u)
#define opcd (*(unsigned int *)0x34034387u)
#define HIRQCycNext (*(unsigned int *)0x340343dcu)
#define oamaddr (*(unsigned int *)0x34048bf0u)
#define ReadHead (*(unsigned int *)0x340a5ff9u)
#define setaramdata ((unsigned int *)(uintptr_t)(*(volatile unsigned int *)0x340a9f78u))
#define setaramdata_SLOT (*(volatile unsigned int *)0x340a9f78u)
#define ramsize (*(unsigned int *)0x341e3b50u)
#define sram ((unsigned int *)(uintptr_t)(*(volatile unsigned int *)0x340a9f88u))
#define sram_SLOT (*(volatile unsigned int *)0x340a9f88u)
#define tempesi (*(unsigned int *)0x34033fd0u)
#define tempedi (*(unsigned int *)0x34033fd4u)
#define tempedx (*(unsigned int *)0x34033fccu)
#define tempebp (*(unsigned int *)0x34033fd8u)

#define StateBackup ((unsigned char *)(uintptr_t)(*(volatile unsigned int *)0x34033e78u))
#define StateBackup_SLOT (*(volatile unsigned int *)0x34033e78u)
#define zsmesg ((unsigned char *)0x340a442bu)
#define sndrot (*(unsigned char *)0x34048012u)
#define spcon (*(unsigned char *)0x340a444fu)
#define spcRam ((unsigned char *)0x34037e80u)
#define DSPMem ((unsigned char *)0x34015520u)
#define C4Enable (*(unsigned char *)0x341e3b28u)
#define SFXEnable (*(unsigned char *)0x341e3b20u)
#define SA1Enable (*(unsigned char *)0x341e3b40u)
#define SA1Status (*(unsigned char *)0x34004780u)
#define SA1RAMArea ((unsigned char *)(uintptr_t)(*(volatile unsigned int *)0x340052fbu))
#define SA1RAMArea_SLOT (*(volatile unsigned int *)0x340052fbu)
#define DSP1Type (*(unsigned char *)0x340a704fu)
#define DSP1COp (*(unsigned char *)0x34001004u)
#define prevoamptr (*(unsigned char *)0x34048e20u)
#define SETAEnable (*(unsigned char *)0x341e3b30u)

#define C4WFXVal (*(short *)0x341e3b58u)
#define C41FXVal (*(short *)0x341e3b90u)
#define Op00Multiplicand (*(short *)0x341e32c0u)
#define Op04Angle (*(short *)0x341e32f8u)
#define Op08X (*(short *)0x341e3318u)
#define Op18X (*(short *)0x341e3340u)
#define Op28X (*(short *)0x341e3368u)
#define Op0CA (*(short *)0x341e3388u)
#define Op02FX (*(short *)0x341e33f8u)
#define Op0AVS (*(short *)0x341e3450u)
#define Op06X (*(short *)0x341e3458u)
#define Op01m (*(short *)0x341e34a8u)
#define Op0DX (*(short *)0x341e3508u)
#define Op03F (*(short *)0x341e35a0u)
#define Op14Zr (*(short *)0x341e3690u)
#define Op0EH (*(short *)0x341e3488u)
#define Op10Coefficient (*(signed short *)0x341e32d8u)

void BackupCVFrame()
{

    unsigned int ramsizew, i;
    unsigned char *curpos;

    curpos = StateBackup + (CBackupPos << 19) + 1024;
    memcpyinc (curpos, zsmesg, PHnum2writecpureg);
    memcpyinc (curpos, &cycpbl, 2*4);
    memcpyinc (curpos, &sndrot, 3019);
    memcpyinc (curpos, wramdata, 8192*16);
    memcpyinc (curpos, vram, 4096*16);

    if (spcon)
    {
	memcpyinc (curpos, spcRam, PHspcsave);
	memcpyinc (curpos, DSPMem, 16*16);
    }

    if (C4Enable)	{ memcpyinc (curpos, C4Ram, 2048*4); }

    if (SFXEnable)	{ memcpyinc (curpos, sfxramdata, 8192*16); }

    if (SA1Enable)
    {
	memcpyinc (curpos, &SA1Mode, PHnum2writesa1reg);
	memcpyinc (curpos, SA1RAMArea, 8192*16);
	memcpyinc (curpos, &SA1Status, 3);
	memcpyinc (curpos, &prevedi, 1*4);
	memcpyinc (curpos, &SA1xpc, 1*4);
	memcpyinc (curpos, ZADDR(SA1RAMArea), 6*4);
	memcpyinc (curpos, &sa1dmaptr, 2*4);
    }

    if (DSP1Type)
    {
	memcpyinc (curpos, &DSP1COp, 70+128);
	memcpyinc (curpos, &C4WFXVal, 7*4+7*8+128);
	memcpyinc (curpos, &C41FXVal, 5*4+128);
	memcpyinc (curpos, &Op00Multiplicand, 3*4+128);
	memcpyinc (curpos, &Op10Coefficient, 4*4+128);
	memcpyinc (curpos, &Op04Angle, 4*4+128);
	memcpyinc (curpos, &Op08X, 5*4+128);
	memcpyinc (curpos, &Op18X, 5*4+128);
	memcpyinc (curpos, &Op28X, 4*4+128);
	memcpyinc (curpos, &Op0CA, 5*4+128);
	memcpyinc (curpos, &Op02FX, 11*4+3*4+28*8+128);
	memcpyinc (curpos, &Op0AVS, 5*4+14*8+128);
	memcpyinc (curpos, &Op06X, 6*4+10*8+4+128);
	memcpyinc (curpos, &Op01m, 4*4+128);
	memcpyinc (curpos, &Op0DX, 6*4+128);
	memcpyinc (curpos, &Op03F, 6*4+128);
	memcpyinc (curpos, &Op14Zr, 9*4+128);
	memcpyinc (curpos, &Op0EH, 4*4+128);
    }

    memcpyinc (curpos, &soundcycleft, 33);
    memcpyinc (curpos, &spc700read, 10*4);
    memcpyinc (curpos, &timer2upd, 1*4);
    memcpyinc (curpos, &xa, 14*4);
    memcpyinc (curpos, &spcnumread, 4);
    memcpyinc (curpos, &spchalted, 4);
    memcpyinc (curpos, &opcd, 6*4);
    memcpyinc (curpos, &HIRQCycNext, 5);
    memcpyinc (curpos, &oamaddr, 14*4);
    memcpyinc (curpos, &prevoamptr, 1);
    memcpyinc (curpos, &ReadHead, 1*4);

    if (SETAEnable)	{ memcpyinc (curpos, setaramdata, 256*16); }

    ramsizew = ramsize >> 4;

    for (i = ramsizew ; i>0 ; i--)
    {
	memcpyinc (curpos, sram, 1*16);
    }

    memcpyinc (curpos, &tempesi, 4);
    memcpyinc (curpos, &tempedi, 4);
    memcpyinc (curpos, &tempedx, 4);
    memcpyinc (curpos, &tempebp, 4);
}

#define Bank0datr8 ((unsigned int *)0x3406e240u)
#define Bank0datr16 ((unsigned int *)0x3406e640u)
#define Bank0datw8 ((unsigned int *)0x3406ea40u)
#define Bank0datw16 ((unsigned int *)0x3406ee40u)
#define xd (*(unsigned int *)0x340a44a0u)
#define DPageR8 (*(unsigned int *)0x34037e5eu)
#define DPageR16 (*(unsigned int *)0x34037e62u)
#define DPageW8 (*(unsigned int *)0x34037e66u)
#define DPageW16 (*(unsigned int *)0x34037e6au)

void UpdateDPageC()
{
    DPageR8 = Bank0datr8[(xd >> 8) & 0xFF];
    DPageR16 = Bank0datr16[(xd >> 8) & 0xFF];
    DPageW8 = Bank0datw8[(xd >> 8) & 0xFF];
    DPageW16 = Bank0datw16[(xd >> 8) & 0xFF];
}

#define SA1xd (*(unsigned int *)0x34004a9eu)
#define SA1DPageR8 (*(unsigned int *)0x34037e6eu)
#define SA1DPageR16 (*(unsigned int *)0x34037e72u)
#define SA1DPageW8 (*(unsigned int *)0x34037e76u)
#define SA1DPageW16 (*(unsigned int *)0x34037e7au)

void SA1UpdateDPageC()
{
    SA1DPageR8 = Bank0datr8[(SA1xd >> 8) & 0xFF];
    SA1DPageR16 = Bank0datr16[(SA1xd >> 8) & 0xFF];
    SA1DPageW8 = Bank0datw8[(SA1xd >> 8) & 0xFF];
    SA1DPageW16 = Bank0datw16[(SA1xd >> 8) & 0xFF];
}

#define PBackupPos (*(unsigned int *)0x34033e7cu)

void RestoreCVFrame()
{
    unsigned int ramsizew, i;
    unsigned char *curpos;

    curpos = StateBackup + (PBackupPos << 19) + 1024;
    memcpyrinc (curpos, zsmesg, PHnum2writecpureg);
    memcpyrinc (curpos, &cycpbl, 2*4);
    memcpyrinc (curpos, &sndrot, 3019);
    memcpyrinc (curpos, wramdata, 8192*16);
    memcpyrinc (curpos, vram, 4096*16);

    if (spcon)
    {
	memcpyrinc (curpos, spcRam, PHspcsave);
	memcpyrinc (curpos, DSPMem, 16*16);
    }

    if (C4Enable)	{ memcpyrinc (curpos, C4Ram, 2048*4); }

    if (SFXEnable)	{ memcpyrinc (curpos, sfxramdata, 8192*16); }

    if (SA1Enable)
    {
	memcpyrinc (curpos, &SA1Mode, PHnum2writesa1reg);
	memcpyrinc (curpos, SA1RAMArea, 8192*16);
	memcpyrinc (curpos, &SA1Status, 3);
	memcpyrinc (curpos, &prevedi, 1*4);
	memcpyrinc (curpos, &SA1xpc, 1*4);
	memcpyrinc (curpos, ZADDR(SA1RAMArea), 6*4);
	memcpyrinc (curpos, &sa1dmaptr, 2*4);
    }

    if (DSP1Type)
    {
	memcpyrinc (curpos, &DSP1COp, 70+128);
	memcpyrinc (curpos, &C4WFXVal, 7*4+7*8+128);
	memcpyrinc (curpos, &C41FXVal, 5*4+128);
	memcpyrinc (curpos, &Op00Multiplicand, 3*4+128);
	memcpyrinc (curpos, &Op10Coefficient, 4*4+128);
	memcpyrinc (curpos, &Op04Angle, 4*4+128);
	memcpyrinc (curpos, &Op08X, 5*4+128);
	memcpyrinc (curpos, &Op18X, 5*4+128);
	memcpyrinc (curpos, &Op28X, 4*4+128);
	memcpyrinc (curpos, &Op0CA, 5*4+128);
	memcpyrinc (curpos, &Op02FX, 11*4+3*4+28*8+128);
	memcpyrinc (curpos, &Op0AVS, 5*4+14*8+128);
	memcpyrinc (curpos, &Op06X, 6*4+10*8+4+128);
	memcpyrinc (curpos, &Op01m, 4*4+128);
	memcpyrinc (curpos, &Op0DX, 6*4+128);
	memcpyrinc (curpos, &Op03F, 6*4+128);
	memcpyrinc (curpos, &Op14Zr, 9*4+128);
	memcpyrinc (curpos, &Op0EH, 4*4+128);
    }

    memcpyrinc (curpos, &soundcycleft, 33);
    memcpyrinc (curpos, &spc700read, 10*4);
    memcpyrinc (curpos, &timer2upd, 1*4);
    memcpyrinc (curpos, &xa, 14*4);
    memcpyrinc (curpos, &spcnumread, 4);
    memcpyrinc (curpos, &spchalted, 4);
    memcpyrinc (curpos, &opcd, 6*4);
    memcpyrinc (curpos, &HIRQCycNext, 5);
    memcpyrinc (curpos, &oamaddr, 14*4);
    memcpyrinc (curpos, &prevoamptr, 1);
    memcpyrinc (curpos, &ReadHead, 1*4);

    if (SETAEnable)	{ memcpyrinc (curpos, setaramdata, 256*16); }

    ramsizew = ramsize >> 4;

    for (i = ramsizew ; i>0 ; i--)
    {
	memcpyrinc (curpos, sram, 1*16);
    }

    memcpyrinc (curpos, &tempesi, 4);
    memcpyrinc (curpos, &tempedi, 4);
    memcpyrinc (curpos, &tempedx, 4);
    memcpyrinc (curpos, &tempebp, 4);

    UpdateDPageC();
    SA1UpdateDPageC();
}
