/* Minimal headless SDL 1.2 stand-in so the ORIGINAL ZSNES linux platform layer builds unmodified. */
#ifndef SDL_SHIM_H
#define SDL_SHIM_H
#include <stdint.h>
typedef uint8_t Uint8; typedef int8_t Sint8; typedef uint16_t Uint16; typedef int16_t Sint16;
typedef uint32_t Uint32; typedef int32_t Sint32;
typedef struct { Uint32 Gmask; } SDL_PixelFormat;
typedef struct SDL_Surface { Uint32 flags; SDL_PixelFormat *format; int w, h; Uint16 pitch; void *pixels; } SDL_Surface;
typedef struct { int scancode; int sym; int mod; Uint16 unicode; } SDL_keysym;
typedef struct { int type; Uint8 gain, state; } SDL_ActiveEvent;
typedef struct { int type; Uint8 state; SDL_keysym keysym; } SDL_KeyboardEvent;
typedef struct { int type; Uint8 state; Uint16 x, y; Sint16 xrel, yrel; } SDL_MouseMotionEvent;
typedef struct { int type; Uint8 button, state; Uint16 x, y; } SDL_MouseButtonEvent;
typedef struct { int type; Uint8 which, axis; Sint16 value; } SDL_JoyAxisEvent;
typedef struct { int type; Uint8 which, ball; Sint16 xrel, yrel; } SDL_JoyBallEvent;
typedef struct { int type; Uint8 which, hat, value; } SDL_JoyHatEvent;
typedef struct { int type; Uint8 which, button, state; } SDL_JoyButtonEvent;
typedef struct { int type; int w, h; } SDL_ResizeEvent;
typedef union { int type; SDL_ActiveEvent active; SDL_KeyboardEvent key; SDL_MouseMotionEvent motion;
  SDL_MouseButtonEvent button; SDL_JoyAxisEvent jaxis; SDL_JoyBallEvent jball; SDL_JoyHatEvent jhat;
  SDL_JoyButtonEvent jbutton; SDL_ResizeEvent resize; } SDL_Event;
typedef struct { int freq; Uint16 format; Uint8 channels, silence; Uint16 samples, padding; Uint32 size;
  void (*callback)(void *userdata, Uint8 *stream, int len); void *userdata; } SDL_AudioSpec;
typedef struct SDL_sem SDL_sem; typedef struct SDL_Thread SDL_Thread; typedef struct SDL_Joystick SDL_Joystick;
enum { SDL_ACTIVEEVENT = 1, SDL_KEYDOWN, SDL_KEYUP, SDL_MOUSEMOTION, SDL_MOUSEBUTTONDOWN, SDL_MOUSEBUTTONUP,
  SDL_JOYAXISMOTION, SDL_JOYBALLMOTION, SDL_JOYHATMOTION, SDL_JOYBUTTONDOWN, SDL_JOYBUTTONUP, SDL_QUIT, SDL_VIDEORESIZE };
enum { SDL_HAT_CENTERED = 0, SDL_HAT_UP = 1, SDL_HAT_RIGHT = 2, SDL_HAT_DOWN = 4, SDL_HAT_LEFT = 8,
  SDL_HAT_RIGHTUP = 3, SDL_HAT_RIGHTDOWN = 6, SDL_HAT_LEFTUP = 9, SDL_HAT_LEFTDOWN = 12 };
#define SDL_INIT_TIMER 1
#define SDL_INIT_AUDIO 0x10
#define SDL_INIT_VIDEO 0x20
#define SDL_INIT_JOYSTICK 0x200
#define SDL_SWSURFACE 0
#define SDL_DOUBLEBUF 0x40000000
#define SDL_FULLSCREEN 0x80000000u
#define SDL_RESIZABLE 0x10
#define SDL_GRAB_ON 1
#define SDL_GRAB_OFF 0
#define SDL_ENABLE 1
#define AUDIO_S16LSB 0x8010
#define KMOD_NUM 0x1000
#define KMOD_ALT 0x300
#define SDL_MUSTLOCK(s) 0
enum {
  SDLK_RETURN = 100,
  SDLK_UP = 101,
  SDLK_DOWN = 102,
  SDLK_LEFT = 103,
  SDLK_RIGHT = 104,
  SDLK_SLASH = 105,
  SDLK_SEMICOLON = 106,
  SDLK_RSHIFT = 107,
  SDLK_LSHIFT = 108,
  SDLK_RIGHTBRACKET = 109,
  SDLK_LEFTBRACKET = 110,
  SDLK_QUOTE = 111,
  SDLK_PERIOD = 112,
  SDLK_MINUS = 113,
  SDLK_EQUALS = 114,
  SDLK_COMMA = 115,
  SDLK_KP0 = 116,
  SDLK_KP1 = 117,
  SDLK_KP2 = 118,
  SDLK_KP3 = 119,
  SDLK_KP4 = 120,
  SDLK_KP5 = 121,
  SDLK_KP6 = 122,
  SDLK_KP7 = 123,
  SDLK_KP8 = 124,
  SDLK_KP9 = 125,
  SDLK_KP_PLUS = 126,
  SDLK_KP_PERIOD = 127,
  SDLK_KP_MULTIPLY = 128,
  SDLK_KP_MINUS = 129,
  SDLK_KP_DIVIDE = 130,
  SDLK_TAB = 131,
  SDLK_SPACE = 132,
  SDLK_PAGEUP = 133,
  SDLK_PAGEDOWN = 134,
  SDLK_HOME = 135,
  SDLK_END = 136,
  SDLK_ESCAPE = 137,
  SDLK_BACKSPACE = 138,
  SDLK_BACKSLASH = 139,
  SDLK_BACKQUOTE = 140
};
int SDL_Init(Uint32); int SDL_InitSubSystem(Uint32); void SDL_QuitSubSystem(Uint32); void SDL_Quit(void);
char *SDL_GetError(void);
SDL_Surface *SDL_SetVideoMode(int, int, int, Uint32);
int SDL_Flip(SDL_Surface *); int SDL_LockSurface(SDL_Surface *); void SDL_UnlockSurface(SDL_Surface *);
void SDL_WM_SetCaption(const char *, const char *); int SDL_WM_GrabInput(int); int SDL_ShowCursor(int);
void SDL_WarpMouse(Uint16, Uint16); Uint8 SDL_GetRelativeMouseState(int *, int *); int SDL_PollEvent(SDL_Event *);
int SDL_NumJoysticks(void); SDL_Joystick *SDL_JoystickOpen(int); const char *SDL_JoystickName(int);
int SDL_JoystickNumAxes(SDL_Joystick *); int SDL_JoystickNumButtons(SDL_Joystick *);
int SDL_JoystickNumHats(SDL_Joystick *); int SDL_JoystickNumBalls(SDL_Joystick *); int SDL_JoystickEventState(int);
int SDL_OpenAudio(SDL_AudioSpec *, SDL_AudioSpec *); void SDL_CloseAudio(void); void SDL_PauseAudio(int);
void SDL_LockAudio(void); void SDL_UnlockAudio(void);
SDL_sem *SDL_CreateSemaphore(Uint32); void SDL_DestroySemaphore(SDL_sem *); int SDL_SemPost(SDL_sem *);
int SDL_SemWaitTimeout(SDL_sem *, Uint32);
SDL_Thread *SDL_CreateThread(int (*fn)(void *), void *data); void SDL_WaitThread(SDL_Thread *, int *);
#endif
