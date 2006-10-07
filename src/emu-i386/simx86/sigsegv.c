/***************************************************************************
 * 
 * All modifications in this file to the original code are
 * (C) Copyright 1992, ..., 2005 the "DOSEMU-Development-Team".
 *
 * for details see file COPYING in the DOSEMU distribution
 *
 *
 *  SIMX86 a Intel 80x86 cpu emulator
 *  Copyright (C) 1997,2001 Alberto Vignani, FIAT Research Center
 *				a.vignani@crf.it
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * Additional copyright notes:
 *
 * 1. The kernel-level vm86 handling was taken out of the Linux kernel
 *  (linux/arch/i386/kernel/vm86.c). This code originaly was written by
 *  Linus Torvalds with later enhancements by Lutz Molgedey and Hans Lermen.
 *
 ***************************************************************************/

#include <stddef.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include "emu86.h"
#include "codegen-arch.h"
#include "trees.h"
#include "dpmi.h"

#include "video.h"
#include "bios.h"
#include "memory.h"
#include "priv.h"
#include "mapping.h"

int TryMemRef = 0;

/* ======================================================================= */

unsigned e_VgaRead(unsigned a, int mode)
{
  unsigned u;
  unsigned char *addr = (unsigned char *)(uintptr_t)a;
  if (mode&MBYTE)
    u = vga_read(addr);
  else {
    u = vga_read_word((unsigned short *)addr);
    if (!(mode&DATA16))
      u |= vga_read_word((unsigned short *)addr+1) << 16;
  }
#ifdef DEBUG_VGA
  e_printf("eVGAEmuFault: VGA read at %08x = %08x mode %x\n",addr,u,mode);
#endif
  return u;
}

void e_VgaWrite(unsigned a, unsigned u, int mode)
{
  unsigned char *addr = (unsigned char *)(uintptr_t)a;
#ifdef DEBUG_VGA
  e_printf("eVGAEmuFault: VGA write %08x at %08x mode %x\n",u,addr,mode);
#endif
  if (mode&MBYTE) {
    vga_write(addr, u);
    return;
  }
  vga_write_word((unsigned short *)addr, u);
  if (mode&DATA16) return;
  vga_write_word((unsigned short *)addr+1, u>>16);
}

static void e_VgaMovs(struct sigcontext_struct *scp, char op, int w16, int dp)
{
  unsigned int rep = (op&2? _ecx : 1);

#ifdef DEBUG_VGA
  e_printf("eVGAEmuFault: Movs ESI=%08x EDI=%08x ECX=%08x\n",_esi,_edi,rep);
#endif
  if (_err&2) {		/* writing from mem or VGA to VGA */
	if ((_esi>=e_vga_base)&&(_esi<e_vga_end)) op |= 4;
	if (op&1) {		/* byte move */
	    if (op&4) goto vga2vgab;
	    while (rep--) {
		e_VgaWrite(_edi,*((char *)_rsi),MBYTE);
		_esi+=dp,_edi+=dp;
	    }
	    if (op&2) _ecx = 0;
	    return;
	}
	else if (w16&1) {	/* word move */
	    if (op&4) goto vga2vgaw;
	    while (rep--) {
		e_VgaWrite(_edi,*((short *)_rsi),DATA16);
		_esi+=dp,_edi+=dp;
	    }
	    if (op&2) _ecx = 0;
	    return;
	}
	else {			/* long move */
	    dp *= 2;
	    if (op&4) goto vga2vgal;
	    while (rep--) {
		e_VgaWrite(_edi,*((long *)_rsi),DATA32);
		_esi+=dp,_edi+=dp;
	    }
	    if (op&2) _ecx = 0;
	    return;
	}
  }
  else {		/* reading from VGA to mem or VGA */
	if ((_edi>=e_vga_base)&&(_edi<e_vga_end)) op |= 4;
	if (op&1) {		/* byte move */
	    if (op&4) {		/* vga2vga */
vga2vgab:
	        while (rep--) {
		  e_VgaWrite(_edi,e_VgaRead(_esi,MBYTE),MBYTE);
		  _esi+=dp,_edi+=dp;
	        }
	    }
	    else while (rep--) {
		*((char *)_rdi) = e_VgaRead(_esi,MBYTE);
		_esi+=dp,_edi+=dp;
	    }
	    if (op&2) _ecx = 0;
	    return;
	}
	else if (w16&1) {	/* word move */
	    if (op&4) {		/* vga2vga */
vga2vgaw:
	        while (rep--) {
		  e_VgaWrite(_edi,e_VgaRead(_esi,DATA16),DATA16);
		  _esi+=dp,_edi+=dp;
	        }
	    }
	    else while (rep--) {
		*((short *)_rdi) = e_VgaRead(_esi,DATA16);
		_esi+=dp,_edi+=dp;
	    }
	    if (op&2) _ecx = 0;
	    return;
	}
	else {			/* long move */
	    dp *= 2;
	    if (op&4) {		/* vga2vga */
vga2vgal:
	        while (rep--) {
		  e_VgaWrite(_edi,e_VgaRead(_esi,DATA32),DATA32);
		  _esi+=dp,_edi+=dp;
	        }
	    }
	    else while (rep--) {
		*((long *)_rdi) = e_VgaRead(_esi,DATA32);
		_esi+=dp,_edi+=dp;
	    }
	    if (op&2) _ecx = 0;
	    return;
	}
  }
}


static int e_vgaemu_fault(struct sigcontext_struct *scp, unsigned page_fault)
{
  int i, j;
  unsigned vga_page = 0, u=0;

  e_vga_base = vga.mem.map[VGAEMU_MAP_BANK_MODE].base_page << 12;
  e_vga_end =  e_vga_base + (vga.mem.map[VGAEMU_MAP_BANK_MODE].pages << 12);

  for (i = 0; i < VGAEMU_MAX_MAPPINGS; i++) {
    j = page_fault - vga.mem.map[i].base_page;
    if (j >= 0 && j < vga.mem.map[i].pages) {
      vga_page = j + vga.mem.map[i].first_page;
#ifdef DEBUG_VGA
      dbug_printf("eVGAEmuFault: found vga_page %x map %d\n",vga_page,i);
#endif
      break;
    }
  }

  if (i == VGAEMU_MAX_MAPPINGS) {
    if (page_fault >= 0xa0 && page_fault < 0xc0) {	/* unmapped VGA area */
#if 0
      u = instr_len(SEG_ADR((unsigned char *), cs, ip));
      LWORD(eip) += u;
#endif
      if (in_dpmi || (u==0)) {
        e_printf("eVGAEmuFault: unknown instruction, page at 0x%05x now writable\n", page_fault << 12);
        vga_emu_protect_page(page_fault, 2);
      }
/**/	leavedos(0x5640);
      return 1;
    }
    else if (page_fault >= 0xc0 && page_fault < (0xc0 + vgaemu_bios.pages)) {	/* ROM area */
#if 0
      u = instr_len(SEG_ADR((unsigned char *), cs, ip));
      LWORD(eip) += u;
#endif
      if (in_dpmi || (u==0)) {
        e_printf("eVGAEmuFault: unknown instruction, converting ROM to RAM at 0x%05x\n", page_fault << 12);
        vga_emu_protect_page(page_fault, 2);
      }
/**/	leavedos(0x5641);
      return 1;
    }
    else {
      e_printf("eVGAEmuFault: unhandled page fault (not in range)\n");
      return 0;
    }
  }
 
  if (vga_page < vga.mem.pages) {
    unsigned char *p;
    unsigned long cxrep;
    int w16;

    vga.mem.dirty_map[vga_page] = 1;

    if (!vga.inst_emu) {
      /* Normal: make the display page writeable after marking it dirty */
      vga_emu_adjust_protection(vga_page, page_fault);
      return 1;
    }  

/**/  e_printf("eVGAEmuFault: trying %08lx, a=%08lx\n",*((long *)_rip),_rdi);

    p = (unsigned char *)_rip;
    if (*p==0x66) w16=1,p++; else w16=0;

    /* Decode the faulting instruction.
     * Hopefully, since the compiled code contains a well-defined subset
     * of the many possibilities for writing a memory location, this
     * decoder can be kept quite small. It is possible, however, that
     * someone accesses the VGA memory with a shift, or a bit set, and
     * this will cause the cpuemu to fail.
     */
    switch (*p) {
	case 0x88:	// write byte
		if ((_err&2)==0) goto badrw;
		if (p[1]!=0x07) goto unimp;
		e_VgaWrite(_edi,_eax,MBYTE);
		_eip = (long)(p+2); break;
	case 0x89:	// write word
		if ((_err&2)==0) goto badrw;
		if (p[1]!=0x07) goto unimp;
		e_VgaWrite(_edi,_eax,(w16? DATA16:DATA32));
		_eip = (long)(p+2); break;
	case 0x8a:	// read byte
		if (_err&2) goto badrw;
		if (p[1]==0x07)
		    *((unsigned char *)&_eax) = e_VgaRead(_edi,MBYTE);
		else if (p[1]==0x17)
		    *((unsigned char *)&_edx) = e_VgaRead(_edi,MBYTE);
		else goto unimp;
		_eip = (long)(p+2); break;
	case 0x8b:	// read word
		if (_err&2) goto badrw;
		if (p[1]!=0x07) goto unimp;
		if (w16)
			*((unsigned short *)&_eax) = e_VgaRead(_edi,DATA16);
		else
			_eax = e_VgaRead(_edi,DATA32);
		_eip = (long)(p+2); break;
	case 0xa4: {	// MOVsb
		int d = (_eflags & EFLAGS_DF? -1:1);
		e_VgaMovs(scp, 1, 0, d);
		_eip = (long)(p+1); } break;
	case 0xa5: {	// MOVsw
		int d = (_eflags & EFLAGS_DF? -1:1);
		e_VgaMovs(scp, 0, w16, d*2);
		_eip = (long)(p+1); } break;
	case 0xaa: {	// STOsb
		int d = (_eflags & EFLAGS_DF? -1:1);
		if ((_err&2)==0) goto badrw;
		e_VgaWrite(_edi,_eax,MBYTE);
		_edi+=d;
		_eip = (long)(p+1); } break;
	case 0xab: {	// STOsw
		int d = (_eflags & EFLAGS_DF? -4:4);
		if ((_err&2)==0) goto badrw;
		if (w16) d>>=1;
		e_VgaWrite(_edi,_eax,(w16? DATA16:DATA32)); _edi+=d;
		_eip = (long)(p+1); } break;
	case 0xac: {	// LODsb
		int d = (_eflags & EFLAGS_DF? -1:1);
		if (_err&2) goto badrw;
		*((char *)&_eax) = e_VgaRead(_esi,MBYTE);
		_esi+=d;
		_eip = (long)(p+1); } break;
	case 0xad: {	// LODsw
		int d = (_eflags & EFLAGS_DF? -4:4);
		if (_err&2) goto badrw;
		if (w16) {
		    d >>= 1;
		    *((short *)&_eax) = e_VgaRead(_esi,DATA16);
		}
		else
		    _eax = e_VgaRead(_esi,DATA32);
		_esi+=d;
		_eip = (long)(p+1); } break;
	case 0xf3: {
		int d = (_eflags & EFLAGS_DF? -1:1);
		if (p[1]==0x66) w16=1,p++;
		if (p[1]==0xaa) {
		    if ((_err&2)==0) goto badrw;
		    cxrep = _ecx;
		    while (cxrep--) {
			e_VgaWrite(_edi,_eax,MBYTE);
			_edi+=d;
		    }
		    _ecx = 0;
		}
		else if (p[1]==0xa4) {
		    e_VgaMovs(scp, 3, 0, d);
		}
		else if (p[1]==0xab) {
		    if ((_err&2)==0) goto badrw;
		    if (w16) {
		      d *= 2;
		      cxrep = _ecx;
		      while (cxrep--) {
			e_VgaWrite(_edi,_eax,DATA16);
			_edi+=d;
		      }
		      _ecx = 0;
		    }
		    else {
		      d *= 4;
		      cxrep = _ecx;
		      while (cxrep--) {
			e_VgaWrite(_edi,_eax,DATA32);
			_edi+=d;
		      }
		      _ecx = 0;
		    }
		}
		else if (p[1]==0xa5) {
		    e_VgaMovs(scp, 2, w16, d*2);
		}
		else goto unimp;
		_eip = (long)(p+2); }
		break;
	default:
/**/  		leavedos(0x5644);
    }
/**/  e_printf("eVGAEmuFault: new eip=%08x\n",_eip);
  }
  return 1;

unimp:
  error("eVGAEmuFault: unimplemented decode instr at %08x: %08lx\n",
	_eip, *((long *)_rip));
  leavedos(0x5643);
badrw:
  error("eVGAEmuFault: bad R/W CR2 bits at %08x: %08lx\n",
	_eip, _err);
  leavedos(0x5643);
}

/* ======================================================================= */
/*
 * DANG_BEGIN_FUNCTION dosemu_fault(int, struct sigcontext_struct);
 *
 * All CPU exceptions (except 13=general_protection from V86 mode,
 * which is directly scanned by the kernel) are handled here.
 *
 * DANG_END_FUNCTION
 */

#define GetSegmentBaseAddress(s)	(((s) >= (MAX_SELECTORS << 3))? 0 :\
					Segments[(s) >> 3].base_addr)

/* this function is called from dosemu_fault */
int e_emu_fault(struct sigcontext_struct *scp)
{
  /* if config.cpuemu==3 (only vm86 emulated) then this function can
     be trapped from within DPMI, and we still must be prepared to
     reset permissions on code pages */
  if (_cs == getsegment(cs) && ((debug_level('e')>1) || (_trapno!=0x0e))) {
    dbug_printf("==============================================================\n");
    dbug_printf("CPU exception 0x%02lx err=0x%08lx cr2=%08lx eip=%08x\n",
	  	 _trapno, _err, _cr2, _eip);
    dbug_printf("==============================================================\n");
    if (debug_level('e')>1) {
	dbug_printf("Host CPU op=%02x\n%s\n",*((unsigned char *)_rip),
	    e_print_scp_regs(scp,(in_dpmi?3:2)));
	dbug_printf("Emul CPU mode=%04x cr2=%08x\n%s\n",
	    TheCPU.mode&0xffff,TheCPU.cr2,e_print_regs());
    }
  }

  /*
   * We are probably asked to stop the execution and run some int
   * set up by the program... so it's a bad idea to just return back.
   * Implemented solution:
   *	for every "sensitive" instruction we store the current PC in
   *	some place, let's say TheCPU.cr2. As we get the exception, we
   *	use this address to return back to the interpreter loop, skipping
   *	the rest of the sequence, and possibly with an error code in
   *	TheCPU.err.
   */

  if (_trapno==0x0e) {
	if (Video->update_screen) {
		if (_cs == getsegment(cs)) {
			unsigned pf = (unsigned)_cr2 >> 12;
			if ((pf & 0xfffe0) == 0xa0) {
				TrapVgaOn = 1;
			}
			/* VGAEMU may also access/protect the LFB */
			if (e_vgaemu_fault(scp,pf) == 1) return 1;
			if ((pf & 0xfffe0) == 0xa0) goto verybad;
		} else {
			if(VGA_EMU_FAULT(scp,code,1)==True) {
				dpmi_check_return(scp);
				return 1;
			}
		}
	}

#ifdef HOST_ARCH_SIM
  }
  return 0;
#else

        /* bit 0 = 1	page protect
         * bit 1 = 1	writing
         * bit 2 = 1	user mode
         * bit 3 = 0	no reserved bit err
         */
	if (_cr2 > stack_init_bot) {
		error("Accessing reserved memory at %08lx\n"
		      "\tMaybe a null segment register\n",_cr2);
		goto verybad;
	}
	if ((_err&0x0f)==0x07) {
		unsigned char *p = (unsigned char *)_eip;
		int codehit = 0;
		register long v;
		/* Got a fault in a write-protected memory page, that is,
		 * a page _containing_code_. 99% of the time we are
		 * hitting data or stack in the same page, NOT code.
		 *
		 * We assume that no other write protections exist and
		 * that no other cause could return an error code of 7.
		 * (this is a very weak assumption indeed)
		 *
		 * _cr2 keeps the address where the code tries to write
		 * _eip keeps the address of the faulting instruction
		 *	(in the code buffer or in the tree)
		 *
		 * Possible instructions we'll find here are (see above):
		 *	8807	movb	%%al,(%%edi)
		 *	(66)8907	mov{wl}	%%{e}ax,(%%edi)
		 *	(f3)(66)a4,a5	movs
		 *	(f3)(66)aa,ab	stos
		 */
#ifdef PROFILE
		PageFaults++;
#endif
		if (debug_level('e')) {
		    v = *((long *)p);
		    __asm__("bswap %0" : "=r" (v) : "0" (v));
		    e_printf("Faulting ops: %08lx\n",v);

		    if (!InCompiledCode)
			e_printf("*\tFault out of code\n");
		    if (e_querymark((void *)_cr2)) {
			e_printf("CODE node hit at %08lx\n",_cr2);
		    }
		    else if (InCompiledCode) {
			e_printf("DATA node hit at %08lx\n",_cr2);
		    }
		}
		/* the page is not unprotected here, the code
		 * linked by Cpatch will do it */
		/* ACH: we can set up a data patch for code
		 * which has not yet been executed! */
		if (Cpatch((void *)_eip)) return 1;
		/* We HAVE to invalidate all the code in the page
		 * if the page is going to be unprotected */
		InvalidateNodePage(_cr2, 0, _eip, &codehit);
		e_munprotect((void *)_cr2, 0);
		/* now go back and perform the faulting op */
		return 1;
	}
  }
  else if (_trapno==0x00) {
	if (InCompiledCode) {
		static char SpecialTailCode[] =	// flags are already back
		    { 0x9c,0xb8,0,0,0,0,0x5a,0xc3,0xf4 };
		TheCPU.err = EXCP00_DIVZ;
		*((unsigned long *)(SpecialTailCode+2)) = TheCPU.cr2;
		_eip = (long)SpecialTailCode;
		return 1;	// restore CPU and jump to our tail code
	}
  }
  return TryMemRef && _trapno != 0x0d;
#endif

verybad:
  /*
   * Too bad, we have to give up.
   */
  dbug_printf("Unhandled CPUEMU exception %02lx\n", _trapno);
  flush_log();
  if (in_vm86) in_vm86=0;	/* otherwise leavedos() complains */
  fatalerr = 5;
  leavedos(fatalerr);		/* shouldn't return */
  return 1;
}

/* ======================================================================= */
