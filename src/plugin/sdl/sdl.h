/*
 * (C) Copyright 1992, ..., 2014 the "DOSEMU-Development-Team".
 *
 * for details see file COPYING in the DOSEMU distribution
 */

void SDL_process_key(SDL_KeyboardEvent keyevent);
extern struct keyboard_client Keyboard_SDL;
int init_SDL_keyb(void *handle, Display *display);
void SDL_process_key_xkb(Display *display, SDL_KeyboardEvent keyevent);
extern int grab_active;
extern struct video_system Video_SDL;
