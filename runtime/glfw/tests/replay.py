#!/usr/bin/env python3
"""Exercise the real C backend and GLFW mapping code with recorded-shaped HID states.

This is a host logic test, not evidence that hardware was read. Only libnx and
unrelated window/allocator glue are substituted; backend and mapping C are real.
"""
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
if os.environ.get("MINESWITCH_BUILD_LIMITED") != "1":
    os.execv(sys.executable, [sys.executable, str(ROOT / "runtime/limit-build.py"),
                             "--", sys.executable, __file__])
SOURCE = ROOT / ".upstream/glfw-switch"
BUILD = ROOT / "runtime/glfw/build/controller-replay"
BUILD.mkdir(parents=True, exist_ok=True)
shared = (SOURCE / "src/input.c").read_text()

def between(text, start, end):
    assert text.count(start) == 1 and text.count(end) == 1
    return text[text.index(start):text.index(end)]

pieces = [between(shared, "// Internal constants for gamepad mapping source types",
                  "// Notifies shared code of a physical key event"),
          between(shared, "void _glfwInputJoystick(_GLFWjoystick* js, int event)",
                  "// Center the cursor in the content area"),
          between(shared, "GLFWAPI int glfwJoystickPresent(int jid)",
                  "GLFWAPI void glfwSetClipboardString")]
backend = (SOURCE / "src/switch_joystick.c").read_text().replace('#include "internal.h"', '')
header = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#define GLFW_INCLUDE_NONE
#include "GLFW/glfw3.h"
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;
typedef int GLFWbool;
#define BIT(n) (1U << (n))
#define BITL(n) (1ULL << (n))
#define _GLFW_PLATFORM_MAPPING_NAME "Switch"
#define _GLFW_POLL_PRESENCE 0
#define _GLFW_POLL_AXES 1
#define _GLFW_POLL_BUTTONS 2
#define _GLFW_POLL_ALL 3
#define _GLFW_REQUIRE_INIT_OR_RETURN(v) do { if (!_glfw.initialized) return v; } while(0)
#define _GLFW_REQUIRE_INIT() do { assert(_glfw.initialized); } while(0)
#define _GLFW_SWAP_POINTERS(a,b) do { void* t = a; a = b; b = t; } while(0)
#define _glfw_calloc calloc
#define _glfw_realloc realloc
#define _glfw_free free
#define _glfw_strdup strdup
#define _glfw_fminf fminf
#define _glfw_fmaxf fmaxf
enum { HidNpadStyleTag_NpadFullKey=1, HidNpadStyleTag_NpadHandheld=2,
    HidNpadStyleTag_NpadJoyDual=4, HidNpadStyleTag_NpadJoyLeft=8,
    HidNpadStyleTag_NpadJoyRight=16, HidNpadStyleSet_NpadStandard=31,
    HidNpadIdType_No1=0, HidNpadIdType_Handheld=32 };
enum { HidNpadButton_A=BIT(0), HidNpadButton_B=BIT(1), HidNpadButton_X=BIT(2),
    HidNpadButton_Y=BIT(3), HidNpadButton_L=BIT(6), HidNpadButton_R=BIT(7),
    HidNpadButton_ZL=BIT(8), HidNpadButton_ZR=BIT(9), HidNpadButton_Plus=BIT(10),
    HidNpadButton_Minus=BIT(11), HidNpadButton_Left=BIT(12), HidNpadButton_Up=BIT(13),
    HidNpadButton_Right=BIT(14), HidNpadButton_Down=BIT(15) };
enum { HidKeyboardKey_UpArrow, HidKeyboardKey_DownArrow, HidKeyboardKey_LeftArrow,
    HidKeyboardKey_RightArrow, HidKeyboardKey_X, HidKeyboardKey_Z, HidKeyboardKey_S,
    HidKeyboardKey_A, HidKeyboardKey_Return, HidKeyboardKey_Escape };
typedef struct { s32 x,y; } HidAnalogStickState;
typedef struct { u64 mask,buttons; u32 style,attributes; HidAnalogStickState sticks[2]; } PadState;
typedef struct { unsigned count; struct { unsigned x,y; } touches[1]; } HidTouchScreenState;
typedef struct { unsigned slot; u32 style; } _GLFWjoystickNX;
typedef struct { uint8_t type,index; int8_t axisScale,axisOffset; } _GLFWmapelement;
typedef struct { char name[128],guid[33]; _GLFWmapelement buttons[15],axes[6]; } _GLFWmapping;
typedef struct { GLFWbool present; float* axes; int axisCount; unsigned char* buttons;
    int buttonCount; unsigned char* hats; int hatCount; char* name; void* userPointer;
    char guid[33]; _GLFWmapping* mapping; _GLFWjoystickNX nx; } _GLFWjoystick;
typedef struct { struct { int width,height; } nx; } _GLFWwindow;
static struct { int initialized; _GLFWjoystick joysticks[16];
    _GLFWmapping* mappings; int mappingCount;
    struct { struct { int hatButtons; } init; } hints;
    struct { GLFWjoystickfun joystick; } callbacks;
    struct { _GLFWwindow* cur_window; } nx; } _glfw;
static PadState fixture[9];
static HidTouchScreenState touchFixture;
static int errors, connects, disconnects, keyEvents, clicks, closes;
static double cursorX,cursorY;
static void _glfwInputError(int code,const char* format,...) { (void)code; (void)format; ++errors; }
void _glfwPlatformUpdateGamepadGUID(char*);
int _glfwPlatformPollJoystick(_GLFWjoystick*,int);
void _glfwUpdateSwitchJoysticks(void);
static void padConfigureInput(unsigned n,u32 style) { assert(n==8 && style==31); }
static void padInitializeWithMask(PadState* p,u64 mask) { memset(p,0,sizeof(*p)); p->mask=mask; }
#define padInitialize(p,id) padInitializeWithMask(p,1ULL<<(id))
static void padUpdate(PadState* p) {
    u64 mask=p->mask; assert(__builtin_popcountll(mask)==1);
    unsigned bit=__builtin_ctzll(mask), slot=bit==32 ? 0 : bit+1;
    assert(slot<9); *p=fixture[slot]; p->mask=mask;
}
static u32 padGetStyleSet(const PadState* p) { return p->style; }
static int padIsConnected(const PadState* p) { return p->attributes&1; }
static u64 padGetButtons(const PadState* p) { return p->buttons; }
static HidAnalogStickState padGetStickPos(const PadState* p,unsigned i) { return p->sticks[i]; }
static void hidInitializeTouchScreen(void) {}
static unsigned hidGetTouchScreenStates(HidTouchScreenState* p,unsigned n) { assert(n==1); *p=touchFixture; return 1; }
static void _glfwInputWindowCloseRequest(_GLFWwindow* w) { assert(w); ++closes; }
static void _glfwInputKey(_GLFWwindow* w,int k,int s,int a,int m) { assert(w); ++keyEvents; }
static void _glfwInputCursorPos(_GLFWwindow* w,double x,double y) { cursorX=x; cursorY=y; }
static void _glfwInputMouseClick(_GLFWwindow* w,int b,int a,int m) { assert(w); clicks+=a==GLFW_PRESS?1:10; }
'''
test = r'''
static void changed(int jid,int event) {
    if(event==GLFW_CONNECTED) { ++connects; assert(glfwJoystickPresent(jid)); }
    else { ++disconnects; assert(!glfwJoystickPresent(jid)); assert(!glfwGetJoystickUserPointer(jid)); }
}
static int presentCount(void) { int n=0; for(int i=0;i<16;i++) n+=glfwJoystickPresent(i); return n; }
int main(void) {
    GLFWgamepadstate state;
    _GLFWwindow window={.nx={640,360}};
    unsetenv("MINESWITCH_GLFW_LEGACY_KEYS");
    _glfwInitSwitchJoysticks();
    _glfw.initialized=1;
    assert(_glfwInitSwitchGamepadMappings());
    assert(!presentCount()); // No phantom controller at startup.
    glfwSetJoystickCallback(changed);
    _glfw.nx.cur_window=&window;
    fixture[0]=(PadState){.style=2,.attributes=1,.buttons=HidNpadButton_B|HidNpadButton_ZL,
        .sticks={{32767,-32767},{-32767,0}}};
    _glfwUpdateSwitchJoysticks();
    assert(presentCount()==1 && connects==1 && !keyEvents);
    assert(strlen(glfwGetJoystickGUID(0))==32);
    assert(glfwGetGamepadState(0,&state));
    assert(state.buttons[GLFW_GAMEPAD_BUTTON_A]==GLFW_PRESS);
    assert(state.buttons[GLFW_GAMEPAD_BUTTON_B]==GLFW_RELEASE);
    assert(state.axes[0]==1 && state.axes[1]==1 && state.axes[2]==-1);
    assert(state.axes[4]==1 && state.axes[5]==-1);
    // Joystick queries read fresh HID state even without glfwPollEvents.
    fixture[0].buttons=HidNpadButton_A|HidNpadButton_Up|HidNpadButton_Right;
    assert(glfwGetGamepadState(0,&state) && state.buttons[GLFW_GAMEPAD_BUTTON_B]==1);
    assert(state.buttons[GLFW_GAMEPAD_BUTTON_A]==0 && state.axes[4]==-1);
    assert(state.buttons[GLFW_GAMEPAD_BUTTON_DPAD_UP] && state.buttons[GLFW_GAMEPAD_BUTTON_DPAD_RIGHT]);
    // A second Npad cannot overwrite or aggregate the first one's axes/buttons.
    fixture[2]=(PadState){.style=1,.attributes=1,.buttons=HidNpadButton_Y,.sticks={{-16384,0},{0,0}}};
    _glfwUpdateSwitchJoysticks();
    assert(presentCount()==2 && connects==2);
    assert(glfwGetGamepadState(1,&state) && state.buttons[GLFW_GAMEPAD_BUTTON_X]==1);
    assert(state.axes[0]<-0.49f && state.axes[0]>-0.51f);
    assert(glfwGetGamepadState(0,&state) && state.axes[0]==1);
    // Mapping updates must change both name lookup and button semantics.
    char mapping[512];
    snprintf(mapping,sizeof(mapping),"%s,Replay override,a:b0,leftx:a0,platform:Switch,",glfwGetJoystickGUID(0));
    assert(glfwUpdateGamepadMappings(mapping));
    assert(!strcmp(glfwGetGamepadName(0),"Replay override"));
    assert(glfwGetGamepadState(0,&state) && state.buttons[GLFW_GAMEPAD_BUTTON_A]==1);
    assert(_glfwInitSwitchGamepadMappings());
    // A style change invalidates the old device before reporting replacement.
    glfwSetJoystickUserPointer(1,&window);
    fixture[2].style=4;
    _glfwUpdateSwitchJoysticks();
    assert(disconnects==1 && connects==3 && presentCount()==2);
    assert(strstr(glfwGetJoystickName(1),"L/R"));
    // Presence queries themselves deliver disconnect, including without a window.
    _glfw.nx.cur_window=NULL;
    fixture[0].attributes=0;
    assert(!glfwJoystickPresent(0) && disconnects==2);
    fixture[0].attributes=1;
    _glfwUpdateSwitchJoysticks();
    assert(presentCount()==2 && connects==4);
    // Single right Joy-Con's actual right stick becomes its sole raw stick.
    fixture[3]=(PadState){.style=16,.attributes=1,.buttons=HidNpadButton_A|BITL(26),
        .sticks={{0,0},{10000,20000}}};
    _glfwUpdateSwitchJoysticks();
    int count;
    const float* raw=glfwGetJoystickAxes(2,&count);
    assert(count==2 && raw[0]>0.30f && raw[1]<-0.60f);
    assert(glfwGetGamepadState(2,&state));
    assert(state.axes[0]>0.60f && state.axes[1]>0.30f && state.axes[2]==0 && state.axes[3]==0);
    assert(state.buttons[GLFW_GAMEPAD_BUTTON_A] && state.buttons[GLFW_GAMEPAD_BUTTON_LEFT_BUMPER]);
    // A single left Joy-Con's real d-pad supplies the rotated face buttons.
    fixture[4]=(PadState){.style=8,.attributes=1,.buttons=HidNpadButton_Left|BITL(25),
        .sticks={{10000,20000},{0,0}}};
    _glfwUpdateSwitchJoysticks();
    assert(glfwGetGamepadState(3,&state) && state.buttons[GLFW_GAMEPAD_BUTTON_A]);
    assert(state.axes[0]<-0.60f && state.axes[1]<-0.30f);
    assert(state.buttons[GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER]);
    _glfw.nx.cur_window=&window;
    touchFixture.count=1; touchFixture.touches[0].x=640; touchFixture.touches[0].y=360;
    _glfwUpdateSwitchJoysticks();
    assert(clicks==1 && cursorX==320 && cursorY==180);
    _glfwUpdateSwitchJoysticks(); assert(clicks==1);
    touchFixture.count=0; _glfwUpdateSwitchJoysticks(); assert(clicks==11);
    _glfwTerminateSwitchJoysticks();
    assert(!presentCount() && !errors);
    free(_glfw.mappings); _glfw.mappings=NULL; _glfw.mappingCount=0;
    memset(fixture,0,sizeof(fixture));
    setenv("MINESWITCH_GLFW_LEGACY_KEYS","1",1);
    _glfwInitSwitchJoysticks(); assert(!presentCount());
    fixture[0]=(PadState){.style=2,.attributes=1,.buttons=HidNpadButton_A};
    _glfwUpdateSwitchJoysticks(); assert(keyEvents==1);
    fixture[0].attributes=0; _glfwUpdateSwitchJoysticks(); assert(keyEvents==2);
    _glfwTerminateSwitchJoysticks();
    puts("CONTROLLER_REPLAY_PASS empty/independent-slots/raw-values/real-mappings/hotplug/single-joycons/touch/legacy/cleanup");
}
'''
translation = BUILD / "controller-replay.c"
translation.write_text(header + "\n".join(pieces) + backend + test)
binary = BUILD / "controller-replay"
subprocess.run(["cc", "-std=gnu11", "-g", "-O1", "-fsanitize=address,undefined",
                "-I", str(SOURCE / "include"), str(translation), "-lm", "-o", str(binary)], check=True)
subprocess.run([str(binary)], check=True)
