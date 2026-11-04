/* Embeds the guest's initial data image (gen/lift/lift_data.bin) without needing an assembler step. */
#if defined(__APPLE__)
#define LD_SEC ".section __DATA,__const\n"
#else
#define LD_SEC ".section .rodata\n"
#endif
__asm__(LD_SEC
        ".balign 16\n"
        ".globl lift_data_bin\nlift_data_bin:\n.globl _lift_data_bin\n_lift_data_bin:\n"
        ".incbin \"gen/lift/lift_data.bin\"\n"
        "lift_data_end:\n"
        ".balign 4\n"
        ".globl lift_data_bin_len\nlift_data_bin_len:\n.globl _lift_data_bin_len\n_lift_data_bin_len:\n"
        ".long lift_data_end - lift_data_bin\n"
        ".text\n");
