/* host input -> hid:USER shared memory (input.c) */
#ifndef PWR_INPUT_H
#define PWR_INPUT_H
#include "kernel.h"
void input_bind(KObject *mem, KObject *pad0);   /* hid:USER GetIPCHandles created these */
void input_key(int sdl_keycode, int down);
void input_pad_button(int pos_button, int down); /* positional: 0=south 1=east 2=west 3=north 4=L 5=R 6=select 7=start 8..11=dpad up/down/left/right */
void input_pad_stick(int x, int y);             /* -32768..32767, y up positive */
void input_touch(int down, int x, int y);       /* bottom-screen pixel coordinates */
void input_update(void);                        /* once per VBlank */
void input_release_all(void);                 /* settings overlay opened: nothing stays held in the game */
int input_stick_active(void);                   /* circle pad deflected (keys or gamepad): flying, for the auto screen layout */
uint32_t input_buttons(void);                   /* current digital buttons (HID bit layout: bit 2 = Select, bit 3 = Start) */
/* free look (freelook.c): the right stick orbits the chase camera when enabled */
void freelook_enable(int on);
void freelook_stick(int x, int y);              /* -32768..32767, y up positive */
/* draw distance multiplier 1..4 (drawdist.c); applied when the game loads its settings */
void drawdist_set(int k);
#endif
