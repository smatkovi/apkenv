#include "common.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <pthread.h>
#include <SDL/SDL.h>
#include <SDL/SDL_audio.h>
#include "../accelerometer/accelerometer.h"

/* Direct dlsym access */
extern void *apkenv_android_dlsym(void *handle, const char *symbol);

typedef jint (*xplane_onload_t)(JavaVM *vm, void *reserved) SOFTFP;
typedef void (*xplane_init_t)(JNIEnv *env, jobject obj, jint width, jint height, jstring dataDir, jstring apkPath) SOFTFP;
typedef void (*xplane_dowork_t)(JNIEnv *env, jobject obj, jobject bitmap) SOFTFP;
typedef void (*xplane_quit_t)(JNIEnv *env, jobject obj) SOFTFP;
typedef void (*xplane_touch_t)(JNIEnv *env, jobject obj, jint finger, jboolean down) SOFTFP;
typedef void (*xplane_touchmove_t)(JNIEnv *env, jobject obj, jint finger, jfloat x, jfloat y) SOFTFP;
typedef void (*xplane_accel_t)(JNIEnv *env, jobject obj, jfloat x, jfloat y, jfloat z) SOFTFP;
typedef void (*xplane_setrunstate_t)(JNIEnv *env, jobject obj, jboolean running) SOFTFP;
typedef void (*xplane_initaudio_t)(void) SOFTFP;
typedef void (*xplane_shutdownaudio_t)(void) SOFTFP;
typedef void (*xplane_getaudiodata_t)(JNIEnv *env, jobject obj, jobject buffer) SOFTFP;

struct SupportModulePriv {
    xplane_onload_t JNI_OnLoad;
    xplane_init_t Init;
    xplane_dowork_t DoWork;
    xplane_quit_t Quit;
    xplane_touch_t OnTouchEvent;
    xplane_touchmove_t OnTouchMoveEvent;
    xplane_accel_t OnAccelEvent;
    xplane_setrunstate_t SetRunState;
    xplane_initaudio_t InitAudio;
    xplane_shutdownaudio_t ShutdownAudio;
    xplane_getaudiodata_t GetAudioData;
    void *lib_handle;
    int want_exit;
    int audio_running;
    short *audio_buffer;
};

static struct SupportModulePriv xplane_priv;
static struct SupportModule *g_self = NULL;

/* Audio ring buffer for smooth playback */
#define AUDIO_RING_SIZE 8192  /* Larger ring buffer */
static short g_audio_ring[AUDIO_RING_SIZE];
static volatile int g_audio_write_pos = 0;
static volatile int g_audio_read_pos = 0;
static int g_audio_sdl_samples = 512;  /* Will be updated by SDL */

/* Audio callback for SDL */
static void audio_callback(void *userdata, Uint8 *stream, int len)
{
    int samples_needed = len / sizeof(short);
    short *out = (short *)stream;
    
    for (int i = 0; i < samples_needed; i++) {
        if (g_audio_read_pos != g_audio_write_pos) {
            out[i] = g_audio_ring[g_audio_read_pos];
            g_audio_read_pos = (g_audio_read_pos + 1) % AUDIO_RING_SIZE;
        } else {
            out[i] = 0;  /* Underrun - silence */
        }
    }
}

static int
xplane_try_init(struct SupportModule *self)
{
    printf("xplane_try_init called\n");
    fflush(stdout);
    
    /* Get the library handle */
    struct JniLibrary *lib = self->global->libraries;
    while (lib != NULL) {
        if (strstr(lib->name, "libxplane.so") != NULL) {
            self->priv->lib_handle = lib->lib;
            break;
        }
        lib = lib->next;
    }
    
    if (self->priv->lib_handle == NULL) {
        printf("xplane_try_init: FAILED - libxplane.so not found\n");
        fflush(stdout);
        return 0;
    }
    
    printf("xplane_try_init: lib_handle = %p\n", self->priv->lib_handle);
    fflush(stdout);
    
    /* Lookup symbols directly using dlsym */
    void *h = self->priv->lib_handle;
    self->priv->JNI_OnLoad = (xplane_onload_t)apkenv_android_dlsym(h, "JNI_OnLoad");
    self->priv->Init = (xplane_init_t)apkenv_android_dlsym(h, "_Z4InitP7_JNIEnvP8_jobjectiiP8_jstringS2_");
    self->priv->DoWork = (xplane_dowork_t)apkenv_android_dlsym(h, "_Z6DoWorkP7_JNIEnvP8_jobjectS2_");
    self->priv->Quit = (xplane_quit_t)apkenv_android_dlsym(h, "_Z4QuitP7_JNIEnvP8_jobject");
    self->priv->OnTouchEvent = (xplane_touch_t)apkenv_android_dlsym(h, "_Z12OnTouchEventP7_JNIEnvP8_jobjectih");
    self->priv->OnTouchMoveEvent = (xplane_touchmove_t)apkenv_android_dlsym(h, "_Z16OnTouchMoveEventP7_JNIEnvP8_jobjectiff");
    self->priv->OnAccelEvent = (xplane_accel_t)apkenv_android_dlsym(h, "_Z12OnAccelEventP7_JNIEnvP8_jobjectfff");
    self->priv->SetRunState = (xplane_setrunstate_t)apkenv_android_dlsym(h, "_Z11SetRunStateP7_JNIEnvP8_jobjecth");
    self->priv->InitAudio = (xplane_initaudio_t)apkenv_android_dlsym(h, "_Z9InitAudiov");
    self->priv->ShutdownAudio = (xplane_shutdownaudio_t)apkenv_android_dlsym(h, "_Z13ShutdownAudiov");
    self->priv->GetAudioData = (xplane_getaudiodata_t)apkenv_android_dlsym(h, "_Z12GetAudioDataP7_JNIEnvP8_jobjectS2_");
    self->priv->audio_running = 0;
    self->priv->audio_buffer = NULL;
    
    printf("xplane_try_init: JNI_OnLoad=%p Init=%p DoWork=%p Quit=%p\n", 
           self->priv->JNI_OnLoad, self->priv->Init, self->priv->DoWork, self->priv->Quit);
    printf("xplane_try_init: InitAudio=%p ShutdownAudio=%p GetAudioData=%p\n",
           self->priv->InitAudio, self->priv->ShutdownAudio, self->priv->GetAudioData);
    fflush(stdout);
    
    if (self->priv->JNI_OnLoad == NULL || self->priv->Init == NULL || self->priv->DoWork == NULL) {
        printf("xplane_try_init: FAILED - missing functions\n");
        fflush(stdout);
        return 0;
    }
    
    printf("xplane_try_init: SUCCESS\n");
    fflush(stdout);
    return 1;
}

static void
xplane_init(struct SupportModule *self, int width, int height, const char *home)
{
    printf("xplane_init called: %dx%d home=%s\n", width, height, home);
    fflush(stdout);
    
    printf("Calling JNI_OnLoad...\n");
    fflush(stdout);
    jint result = self->priv->JNI_OnLoad(VM_M, NULL);
    printf("JNI_OnLoad returned %d\n", result);
    fflush(stdout);
    
    /* Create jstrings for data directory and APK path */
    jstring dataDir = GLOBAL_M->env->NewStringUTF(ENV_M, home);
    char apk_full_path[PATH_MAX];
    realpath(GLOBAL_M->apk_filename, apk_full_path);
    printf("APK full path: %s\n", apk_full_path);
    jstring apkPath = GLOBAL_M->env->NewStringUTF(ENV_M, apk_full_path);
    
    printf("Calling Init(%d, %d, dataDir, apkPath)...\n", width, height);
    fflush(stdout);
    self->priv->Init(ENV_M, NULL, width, height, apkPath, dataDir);
    printf("Init done\n");
    fflush(stdout);
    
    /* Set run state to running */
    if (self->priv->SetRunState) {
        self->priv->SetRunState(ENV_M, NULL, 1);
    }
    
    /* Save self for audio callback - must be set before SDL_OpenAudio */
    g_self = self;
    
    /* Initialize audio */
    if (self->priv->InitAudio) {
        printf("Calling InitAudio...\n");
        fflush(stdout);
        self->priv->InitAudio();
        printf("InitAudio done\n");
        fflush(stdout);
        
        /* Start SDL audio */
        if (self->priv->GetAudioData) {
            SDL_AudioSpec wanted, obtained;
            
            /* Allocate audio buffer for GetAudioData (512 samples stereo) */
            self->priv->audio_buffer = (short *)malloc(512 * 2 * sizeof(short));
            
            wanted.freq = 22050;
            wanted.format = AUDIO_S16SYS;
            wanted.channels = 2;
            wanted.samples = 512;
            wanted.callback = audio_callback;
            wanted.userdata = self->priv;
            
            if (SDL_OpenAudio(&wanted, &obtained) == 0) {
                printf("SDL Audio opened: freq=%d channels=%d samples=%d\n",
                       obtained.freq, obtained.channels, obtained.samples);
                fflush(stdout);
                g_audio_sdl_samples = obtained.samples;
                SDL_PauseAudio(0);  /* Start playing */
                self->priv->audio_running = 1;
            } else {
                printf("SDL_OpenAudio failed: %s\n", SDL_GetError());
                fflush(stdout);
            }
        }
    }
    
    /* Initialize accelerometer */
    apkenv_accelerometer_init();
    
    self->priv->want_exit = 0;
}

static void
xplane_input(struct SupportModule *self, int event, int x, int y, int finger)
{
    if (self->priv->OnTouchEvent && self->priv->OnTouchMoveEvent) {
        if (event == ACTION_DOWN) {
            self->priv->OnTouchEvent(ENV_M, NULL, finger, 1);
            self->priv->OnTouchMoveEvent(ENV_M, NULL, finger, (float)x, (float)y);
        } else if (event == ACTION_UP) {
            self->priv->OnTouchEvent(ENV_M, NULL, finger, 0);
        } else if (event == ACTION_MOVE) {
            self->priv->OnTouchMoveEvent(ENV_M, NULL, finger, (float)x, (float)y);
        }
    }
}

static void xplane_key_input(struct SupportModule *self, int event, int keycode, int unicode) {}

static void
xplane_update(struct SupportModule *self)
{
    /* Read accelerometer and send to X-Plane */
    if (self->priv->OnAccelEvent) {
        float x, y, z;
        if (apkenv_accelerometer_get(&x, &y, &z)) {
            /* Fix axis orientation: negate Y for correct up/down */
            self->priv->OnAccelEvent(ENV_M, NULL, x, -y, z);
        }
    }
    
    /* Pump audio data into ring buffer */
    if (self->priv->GetAudioData && self->priv->audio_buffer && self->priv->audio_running) {
        /* Check how much space is in ring buffer */
        int write_pos = g_audio_write_pos;
        int read_pos = g_audio_read_pos;
        int available = (read_pos - write_pos - 1 + AUDIO_RING_SIZE) % AUDIO_RING_SIZE;
        
        /* If we have room for at least 512 stereo samples (1024 shorts), fetch more */
        while (available >= 1024) {
            /* Get audio data (512 samples stereo = 1024 shorts) */
            self->priv->GetAudioData(ENV_M, NULL, (jobject)self->priv->audio_buffer);
            
            /* Copy to ring buffer */
            short *src = self->priv->audio_buffer;
            for (int i = 0; i < 1024; i++) {
                g_audio_ring[write_pos] = src[i];
                write_pos = (write_pos + 1) % AUDIO_RING_SIZE;
            }
            g_audio_write_pos = write_pos;
            
            /* Recalculate available space */
            read_pos = g_audio_read_pos;
            available = (read_pos - write_pos - 1 + AUDIO_RING_SIZE) % AUDIO_RING_SIZE;
        }
    }
    
    if (self->priv->DoWork) {
        self->priv->DoWork(ENV_M, NULL, NULL);
    }
}

static void
xplane_deinit(struct SupportModule *self)
{
    printf("xplane_deinit called\n");
    fflush(stdout);
    
    /* Stop SDL audio */
    if (self->priv->audio_running) {
        SDL_PauseAudio(1);
        SDL_CloseAudio();
        self->priv->audio_running = 0;
    }
    
    /* Free audio buffer */
    if (self->priv->audio_buffer) {
        free(self->priv->audio_buffer);
        self->priv->audio_buffer = NULL;
    }
    
    /* Shutdown audio first */
    if (self->priv->ShutdownAudio) {
        self->priv->ShutdownAudio();
    }
    
    if (self->priv->Quit) {
        self->priv->Quit(ENV_M, NULL);
    }
}

static void xplane_pause(struct SupportModule *self) {
    if (self->priv->SetRunState) {
        self->priv->SetRunState(ENV_M, NULL, 0);
    }
}

static void xplane_resume(struct SupportModule *self) {
    if (self->priv->SetRunState) {
        self->priv->SetRunState(ENV_M, NULL, 1);
    }
}

static int xplane_requests_exit(struct SupportModule *self) {
    return self->priv->want_exit;
}

/* Manual non-static init function for cross-compilation */
int apkenv_module_init_xplane(int version, struct SupportModule *module) {
    if (version != APKENV_MODULE_VERSION) {
        return APKENV_MODULE_VERSION;
    }
    module->priv = &xplane_priv;
    module->priority = MODULE_PRIORITY_GAME;
    module->try_init = xplane_try_init;
    module->init = xplane_init;
    module->input = xplane_input;
    module->key_input = xplane_key_input;
    module->update = xplane_update;
    module->deinit = xplane_deinit;
    module->pause = xplane_pause;
    module->resume = xplane_resume;
    module->requests_exit = xplane_requests_exit;
    return APKENV_MODULE_VERSION;
}
