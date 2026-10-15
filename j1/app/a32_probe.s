@ Code A32 (« mode ARM ») exécuté par l'app J1 avant V8 : le JIT et les builtins de V8
@ ARM32 sont en A32, alors que Windows on ARM est documenté « Thumb-2 uniquement ».
@ Assemblé en octets par gen_a32.ps1 (a32_probe.h), copié dans une page RX à l'exécution
@ et appelé par une adresse paire (bit Thumb à 0) : BLX bascule alors en mode ARM.
    .syntax unified
    .arm
    .text

@ int a32_ret42(void) : retour simple.
a32_ret42:
    mov     r0, #42
    bx      lr

@ uint32_t a32_loop(uint32_t n) : n additions VFP en A32 (d0 += 1.0), puis renvoie d0
@ converti en entier. Assez long pour subir interruptions et préemptions en mode ARM.
a32_loop:
    vmov.f64    d1, #1.0
    vsub.f64    d0, d1, d1
1:  vadd.f64    d0, d0, d1
    subs        r0, r0, #1
    bne         1b
    vcvt.u32.f64 s0, d0
    vmov        r0, s0
    bx      lr

@ int a32_call(int (*thumb_fn)(int), int x) : appel d'une fonction Thumb (C++) depuis
@ l'A32 puis retour en A32, comme les builtins de V8 qui appellent le runtime C++.
a32_call:
    push    {r4, lr}
    mov     r4, r0
    mov     r0, r1
    blx     r4
    add     r0, r0, #1
    pop     {r4, pc}

@ uint32_t a32_touch(uint8_t* p, uint32_t pages) : lit puis écrit le premier octet de chaque
@ page de 4 Ko d'une zone jamais touchée, donc un défaut de page (mémoire à zéro à la demande)
@ par page, en A32. Renvoie la somme des octets lus : 0 attendu.
a32_touch:
    mov     r2, #0
    mov     r3, #1
1:  ldrb    r12, [r0]
    add     r2, r2, r12
    strb    r3, [r0]
    add     r0, r0, #4096
    subs    r1, r1, #1
    bne     1b
    mov     r0, r2
    bx      lr

@ uint32_t a32_stack(uint32_t pages) : descend la pile page par page en écrivant dans chacune,
@ ce qui franchit les pages de garde de la pile Windows en A32, puis la restaure.
@ Renvoie le nombre de pages parcourues.
a32_stack:
    mov     r2, r0
    mov     r3, #0
1:  sub     sp, sp, #4096
    str     r2, [sp]
    add     r3, r3, #1
    subs    r2, r2, #1
    bne     1b
    add     sp, sp, r3, lsl #12
    mov     r0, r3
    bx      lr
