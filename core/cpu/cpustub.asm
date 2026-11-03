; 65816 opcode stubs: asm table entry -> C (cpuc.c) -> the original "endloop" dispatch.
%include "macros.mac"

EXTSYM cpucycle,cycpbl,cycpblt,opcjmptab
EXTSYM zs_cpu_op

SECTION .bss
GLOBAL zs_R
zs_R resd 8
GLOBAL zs_mcall_fn
zs_mcall_fn resd 1
zs_run_esp resd 1

SECTION .text
; void zs_mcall(void *fn): load the machine registers from zs_R, call fn, store them back
GLOBAL zs_mcall
zs_mcall:
    push ebx
    push esi
    push edi
    push ebp
    mov eax,[esp+20]
    mov [zs_mcall_fn],eax
    mov ecx,[zs_R+4]
    mov edx,[zs_R+8]
    mov ebx,[zs_R+12]
    mov esi,[zs_R+16]
    mov edi,[zs_R+20]
    mov ebp,[zs_R+24]
    mov eax,[zs_R]
    call dword [zs_mcall_fn]
    mov [zs_R],eax
    mov [zs_R+4],ecx
    mov [zs_R+8],edx
    mov [zs_R+12],ebx
    mov [zs_R+16],esi
    mov [zs_R+20],edi
    mov [zs_R+24],ebp
    pop ebp
    pop edi
    pop esi
    pop ebx
    ret

%macro endloop_b 0
    mov bl,[esi]
    inc esi
    sub dh,[cpucycle+ebx]
    jc %%cpuover
    jmp dword near [edi+ebx*4]
%%cpuover
    ret
%endmacro

%macro endloop_c 0
    sub dword[cycpbl],55
    jnc %%skipallspc
    mov eax,[cycpblt]
    mov bl,[ebp]
    add dword[cycpbl],eax
    inc ebp
    call dword near [opcjmptab+ebx*4]
    xor ebx,ebx
%%skipallspc
    mov bl,[esi]
    inc esi
    sub dh,[cpucycle+ebx]
    jc %%cpuover
    jmp dword near [edi+ebx*4]
%%cpuover
    ret
%endmacro

%macro cstub_enter 2
    mov [zs_R],eax
    mov [zs_R+4],ecx
    mov [zs_R+8],edx
    mov [zs_R+12],ebx
    mov [zs_R+16],esi
    mov [zs_R+20],edi
    mov [zs_R+24],ebp
    push dword %2
    push dword %1
    call zs_cpu_op
    add esp,8
    mov eax,[zs_R]
    mov ecx,[zs_R+4]
    mov edx,[zs_R+8]
    mov ebx,[zs_R+12]
    mov esi,[zs_R+16]
    mov edi,[zs_R+20]
    mov ebp,[zs_R+24]
%endmacro

%assign zi 0
%rep 256
zs_cstub_b_ %+ zi:
    cstub_enter zi,0
    endloop_b
zs_cstub_c_ %+ zi:
    cstub_enter zi,1
    endloop_c
%assign zi zi+1
%endrep

SECTION .data
GLOBAL zs_cstub_b_tab,zs_cstub_c_tab
zs_cstub_b_tab:
%assign zi 0
%rep 256
    dd zs_cstub_b_ %+ zi
%assign zi zi+1
%endrep
zs_cstub_c_tab:
%assign zi 0
%rep 256
    dd zs_cstub_c_ %+ zi
%assign zi zi+1
%endrep

SECTION .text
; capture routine used by the differential test: every opcode-table entry points here
GLOBAL zs_capture_tab
zs_capture:
    ret
SECTION .data
zs_capture_tab:
%rep 256
    dd zs_capture
%endrep
SECTION .text

; void zs_cpu_run(void *entry, const regs_t *in, regs_t *out)
; runs one handler with the given machine registers; the handler's endloop ends up in zs_capture (ret) or its own ret
GLOBAL zs_cpu_run
zs_cpu_run:
    pushad
    mov [zs_run_esp],esp
    mov eax,[esp+36]     ; entry
    mov ebx,[esp+40]     ; in
    mov [zs_mcall_fn],eax
    mov ecx,[ebx+4]
    mov edx,[ebx+8]
    mov esi,[ebx+16]
    mov edi,[ebx+20]
    mov ebp,[ebx+24]
    mov eax,[ebx]
    mov ebx,[ebx+12]
    call dword [zs_mcall_fn]
    mov esp,[zs_run_esp]
    mov [zs_R],eax
    mov [zs_R+4],ecx
    mov [zs_R+8],edx
    mov [zs_R+12],ebx
    mov [zs_R+16],esi
    mov [zs_R+20],edi
    mov [zs_R+24],ebp
    mov eax,[esp+44]     ; out
    mov ecx,[zs_R]
    mov [eax],ecx
    mov ecx,[zs_R+4]
    mov [eax+4],ecx
    mov ecx,[zs_R+8]
    mov [eax+8],ecx
    mov ecx,[zs_R+12]
    mov [eax+12],ecx
    mov ecx,[zs_R+16]
    mov [eax+16],ecx
    mov ecx,[zs_R+20]
    mov [eax+20],ecx
    mov ecx,[zs_R+24]
    mov [eax+24],ecx
    popad
    ret

; ---- DMA/HDMA differential fuzz support: recording read/write handlers --------------------------
SECTION .bss
GLOBAL zs_fz_log, zs_fz_n, zs_fz_mem
zs_fz_log resd 8*4096
zs_fz_n resd 1
zs_fz_mem resb 65536+8
SECTION .text

%macro FZLOG 1
    pushfd
    push edx
    push edi
    mov edi,[zs_fz_n]
    cmp edi,4096
    jae %%full
    inc dword[zs_fz_n]
    shl edi,5
    mov dword[zs_fz_log+edi],%1
    mov [zs_fz_log+edi+4],eax
    mov [zs_fz_log+edi+8],ecx
    mov [zs_fz_log+edi+12],ebx
    mov [zs_fz_log+edi+16],esi
    mov [zs_fz_log+edi+20],edx
    mov dword[zs_fz_log+edi+24],0
    mov dword[zs_fz_log+edi+28],0
%%full:
    pop edi
    pop edx
    popfd
%endmacro

GLOBAL zs_fz_rd8, zs_fz_rd16
zs_fz_rd8:
    FZLOG 1
    push ebx
    and ebx,0FFh
    imul ebx,ebx,31
    add ebx,ecx
    and ebx,0FFFFh
    mov al,[zs_fz_mem+ebx]
    pop ebx
    xor ebx,ebx
    ret
zs_fz_rd16:
    FZLOG 2
    push ebx
    and ebx,0FFh
    imul ebx,ebx,31
    add ebx,ecx
    and ebx,0FFFFh
    mov ax,[zs_fz_mem+ebx]
    pop ebx
    xor ebx,ebx
    ret

%macro FZWR 2
GLOBAL %1
%1:
    FZLOG %2
    ret
%endmacro
FZWR zs_fz_wr0,16
FZWR zs_fz_wr1,17
FZWR zs_fz_wr2,18
FZWR zs_fz_wr3,19
FZWR zs_fz_wr4,20
FZWR zs_fz_wr5,21
FZWR zs_fz_wr6,22
FZWR zs_fz_wr7,23

; void zs_fz_callasm(void *fn, uint32 eax): call an asm routine preserving all C registers
GLOBAL zs_fz_callasm
zs_fz_callasm:
    pushad
    mov ecx,[esp+36]
    mov eax,[esp+40]
    call ecx
    popad
    ret
