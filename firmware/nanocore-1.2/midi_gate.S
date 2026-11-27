/* midi_gate.S - haak op de native MIDI-berichtfunctie van firmware 1.2 (0x4002db26).
 * Die functie krijgt elk ontleed bericht van beide MIDI-poorten (USB en BLE):
 *   a0 = poort, a1 = status, a2 = data1, a3 = data2, a4 = data2 aanwezig.
 * De eerste instructie daar is `jal t0,0x4000c3a4` (register-save-helper). De haak
 * vervangt die door een sprong hierheen; ra is nog die van de aanroeper.
 * nc_midi_in() geeft 1 = door HARDCORE overgenomen (dan direct terug naar de aanroeper),
 * 0 = naar de native code: proloog exact naspelen (t0 = hervatadres, dan de helper). */
.option norelax
.section .text
.global nc_midi_gate
nc_midi_gate:
 addi sp,sp,-32
 sw ra,28(sp)
 sw a0,24(sp)
 sw a1,20(sp)
 sw a2,16(sp)
 sw a3,12(sp)
 sw a4,8(sp)
 call nc_midi_in
 mv t2,a0
 lw ra,28(sp)
 lw a0,24(sp)
 lw a1,20(sp)
 lw a2,16(sp)
 lw a3,12(sp)
 lw a4,8(sp)
 addi sp,sp,32
 beqz t2,1f
 ret
1: li t0,0x4002db2a
 li t1,0x4000c3a4
 jr t1
