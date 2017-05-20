#include <GL/glew.h>
#include <SDL.h>
#include <SDL_opengl.h>
#include <soundio/soundio.h>
#include "core/assert.h"
#include "core/log.h"
#include "core/option.h"
#include "core/profiler.h"
#include "core/ringbuf.h"
#include "emulator.h"
#include "host.h"
#include "sys/filesystem.h"
#include "tracer.h"

DEFINE_OPTION_INT(audio, 1, "Enable audio");
DEFINE_OPTION_INT(latency, 100, "Preferred audio latency in ms");
DEFINE_OPTION_INT(help, 0, "Show help");

#define AUDIO_FREQ 44100
#define VIDEO_DEFAULT_WIDTH 640
#define VIDEO_DEFAULT_HEIGHT 480
#define INPUT_MAX_CONTROLLERS 4

/*
 * sdl host implementation
 */
struct sdl_host {
  struct host;

  struct SDL_Window *win;
  int closed;
  int video_width;
  int video_height;

  struct SoundIo *soundio;
  struct SoundIoDevice *soundio_device;
  struct SoundIoOutStream *soundio_stream;
  struct ringbuf *audio_frames;

  int key_map[K_NUM_KEYS];
  SDL_GameController *controllers[INPUT_MAX_CONTROLLERS];
};

static void host_poll_events(struct sdl_host *host);

struct sdl_host *g_host;

/*
 * audio
 */
static int audio_read_frames(struct sdl_host *host, void *data,
                             int num_frames) {
  int available = ringbuf_available(host->audio_frames);
  int size = MIN(available, num_frames * 4);
  CHECK_EQ(size % 4, 0);

  void *read_ptr = ringbuf_read_ptr(host->audio_frames);
  memcpy(data, read_ptr, size);
  ringbuf_advance_read_ptr(host->audio_frames, size);

  return size / 4;
}

static void audio_write_frames(struct sdl_host *host, const void *data,
                               int num_frames) {
  int remaining = ringbuf_remaining(host->audio_frames);
  int size = MIN(remaining, num_frames * 4);
  CHECK_EQ(size % 4, 0);

  void *write_ptr = ringbuf_write_ptr(host->audio_frames);
  memcpy(write_ptr, data, size);
  ringbuf_advance_write_ptr(host->audio_frames, size);
}

static int audio_available_frames(struct sdl_host *host) {
  int available = ringbuf_available(host->audio_frames);
  return available / 4;
}

static int audio_buffer_low(struct sdl_host *host) {
  if (!host->soundio) {
    return 0;
  }

  int low_water_mark =
      (int)((float)AUDIO_FREQ * (host->soundio_stream->software_latency));
  return audio_available_frames(host) <= low_water_mark;
}

static void audio_write_callback(struct SoundIoOutStream *outstream,
                                 int frame_count_min, int frame_count_max) {
  struct sdl_host *host = outstream->userdata;
  const struct SoundIoChannelLayout *layout = &outstream->layout;
  struct SoundIoChannelArea *areas;
  int err;

  static uint32_t tmp[AUDIO_FREQ];
  int frames_available = audio_available_frames(host);
  int frames_silence = frame_count_max - frames_available;
  int frames_remaining = frames_available + frames_silence;

  while (frames_remaining > 0) {
    int frame_count = frames_remaining;

    if ((err =
             soundio_outstream_begin_write(outstream, &areas, &frame_count))) {
      LOG_WARNING("Error writing to output stream: %s", soundio_strerror(err));
      break;
    }

    if (!frame_count) {
      break;
    }

    for (int frame = 0; frame < frame_count;) {
      int n = MIN(frame_count - frame, array_size(tmp));

      if (frames_available > 0) {
        /* batch read frames from ring buffer */
        n = audio_read_frames(host, tmp, n);
        frames_available -= n;
      } else {
        /* write out silence */
        memset(tmp, 0, n * sizeof(tmp[0]));

        LOG_WARNING("wrote out %d frames of silence", n);
      }

      /* copy frames to output stream */
      int16_t *samples = (int16_t *)tmp;

      for (int channel = 0; channel < layout->channel_count; channel++) {
        struct SoundIoChannelArea *area = &areas[channel];

        for (int i = 0; i < n; i++) {
          int16_t *ptr = (int16_t *)(area->ptr + area->step * (frame + i));
          *ptr = samples[channel + 2 * i];
        }
      }

      frame += n;
    }

    if ((err = soundio_outstream_end_write(outstream))) {
      LOG_WARNING("Error writing to output stream: %s", soundio_strerror(err));
      break;
    }

    frames_remaining -= frame_count;
  }
}

static void audio_underflow_callback(struct SoundIoOutStream *outstream) {
  LOG_WARNING("audio_underflow_callback");
}

void audio_push(struct host *base, const int16_t *data, int num_frames) {
  struct sdl_host *host = (struct sdl_host *)base;

  if (!host->soundio) {
    return;
  }

  audio_write_frames(host, data, num_frames);
}

static void audio_shutdown(struct sdl_host *host) {
  if (host->soundio_stream) {
    soundio_outstream_destroy(host->soundio_stream);
  }

  if (host->soundio_device) {
    soundio_device_unref(host->soundio_device);
  }

  if (host->soundio) {
    soundio_destroy(host->soundio);
  }

  if (host->audio_frames) {
    ringbuf_destroy(host->audio_frames);
  }
}

static int audio_init(struct sdl_host *host) {
  if (!OPTION_audio) {
    return 1;
  }

  int err;

  host->audio_frames = ringbuf_create(AUDIO_FREQ * 4);

  /* connect to a soundio backend */
  {
    struct SoundIo *soundio = host->soundio = soundio_create();

    if (!soundio) {
      LOG_WARNING("Error creating soundio instance");
      return 0;
    }

    if ((err = soundio_connect(soundio))) {
      LOG_WARNING("Error connecting soundio: %s", soundio_strerror(err));
      return 0;
    }

    soundio_flush_events(soundio);
  }

  /* connect to an output device */
  {
    int default_out_device_index =
        soundio_default_output_device_index(host->soundio);

    if (default_out_device_index < 0) {
      LOG_WARNING("Error finding audio output device");
      return 0;
    }

    struct SoundIoDevice *device = host->soundio_device =
        soundio_get_output_device(host->soundio, default_out_device_index);

    if (!device) {
      LOG_WARNING("Error creating output device instance");
      return 0;
    }
  }

  /* create an output stream that matches the AICA output format
     44.1 khz, 2 channel, S16 LE */
  {
    struct SoundIoOutStream *outstream = host->soundio_stream =
        soundio_outstream_create(host->soundio_device);
    outstream->userdata = host;
    outstream->format = SoundIoFormatS16NE;
    outstream->sample_rate = AUDIO_FREQ;
    outstream->write_callback = &audio_write_callback;
    outstream->underflow_callback = &audio_underflow_callback;
    outstream->software_latency = OPTION_latency / 1000.0;

    if ((err = soundio_outstream_open(outstream))) {
      LOG_WARNING("Error opening audio device: %s", soundio_strerror(err));
      return 0;
    }

    if ((err = soundio_outstream_start(outstream))) {
      LOG_WARNING("Error starting device: %s", soundio_strerror(err));
      return 0;
    }
  }

  LOG_INFO("audio backend created, latency %.2f",
           host->soundio_stream->software_latency);

  return 1;
}

/*
 * video
 */
static void video_context_destroyed(struct sdl_host *host) {
  if (!host->video_context_destroyed) {
    return;
  }

  host->video_context_destroyed(host->userdata);
}

static void video_context_reset(struct sdl_host *host) {
  if (!host->video_context_reset) {
    return;
  }

  host->video_context_reset(host->userdata);
}

static void video_resized(struct sdl_host *host) {
  if (!host->video_resized) {
    return;
  }

  host->video_resized(host->userdata);
}

void video_gl_make_current(struct host *base, gl_context_t ctx) {
  struct sdl_host *host = (struct sdl_host *)base;
  int res = SDL_GL_MakeCurrent(host->win, ctx);
  CHECK_EQ(res, 0);
}

void video_gl_destroy_context(struct host *base, gl_context_t ctx) {
  SDL_GL_DeleteContext(ctx);
}

gl_context_t video_gl_create_context_from(struct host *base,
                                          gl_context_t from) {
  struct sdl_host *host = (struct sdl_host *)base;

  SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
  int res = SDL_GL_MakeCurrent(host->win, from);
  CHECK_EQ(res, 0);

  return video_gl_create_context(base);
}

gl_context_t video_gl_create_context(struct host *base) {
  struct sdl_host *host = (struct sdl_host *)base;

  /* need at least a 3.3 core context for our shaders */
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);

  SDL_GLContext ctx = SDL_GL_CreateContext(host->win);
  CHECK_NOTNULL(ctx, "OpenGL context creation failed: %s", SDL_GetError());

  /* disable vsync */
  int res = SDL_GL_SetSwapInterval(0);
  CHECK_EQ(res, 0, "Failed to disable vsync");

  /* link in gl functions at runtime */
  glewExperimental = GL_TRUE;
  GLenum err = glewInit();
  CHECK_EQ(err, GLEW_OK, "GLEW initialization failed: %s",
           glewGetErrorString(err));
  glGetError();

  return (gl_context_t)ctx;
}

int video_gl_supports_multiple_contexts(struct host *base) {
  return 1;
}

int video_height(struct host *base) {
  struct sdl_host *host = (struct sdl_host *)base;
  return host->video_height;
}

int video_width(struct host *base) {
  struct sdl_host *host = (struct sdl_host *)base;
  return host->video_width;
}

static void video_shutdown(struct sdl_host *host) {}

static int video_init(struct sdl_host *host) {
  return 1;
}

/*
 * input
 */
static enum keycode translate_sdl_key(SDL_Keysym keysym) {
  enum keycode out = K_UNKNOWN;

  if (keysym.sym >= SDLK_SPACE && keysym.sym <= SDLK_z) {
    /* this range maps 1:1 with ASCII chars */
    out = (enum keycode)keysym.sym;
  } else {
    switch (keysym.sym) {
      case SDLK_CAPSLOCK:
        out = K_CAPSLOCK;
        break;
      case SDLK_RETURN:
        out = K_RETURN;
        break;
      case SDLK_ESCAPE:
        out = K_ESCAPE;
        break;
      case SDLK_BACKSPACE:
        out = K_BACKSPACE;
        break;
      case SDLK_TAB:
        out = K_TAB;
        break;
      case SDLK_PAGEUP:
        out = K_PAGEUP;
        break;
      case SDLK_PAGEDOWN:
        out = K_PAGEDOWN;
        break;
      case SDLK_DELETE:
        out = K_DELETE;
        break;
      case SDLK_RIGHT:
        out = K_RIGHT;
        break;
      case SDLK_LEFT:
        out = K_LEFT;
        break;
      case SDLK_DOWN:
        out = K_DOWN;
        break;
      case SDLK_UP:
        out = K_UP;
        break;
      case SDLK_LCTRL:
        out = K_LCTRL;
        break;
      case SDLK_LSHIFT:
        out = K_LSHIFT;
        break;
      case SDLK_LALT:
        out = K_LALT;
        break;
      case SDLK_LGUI:
        out = K_LGUI;
        break;
      case SDLK_RCTRL:
        out = K_RCTRL;
        break;
      case SDLK_RSHIFT:
        out = K_RSHIFT;
        break;
      case SDLK_RALT:
        out = K_RALT;
        break;
      case SDLK_RGUI:
        out = K_RGUI;
        break;
      case SDLK_F1:
        out = K_F1;
        break;
      case SDLK_F2:
        out = K_F2;
        break;
      case SDLK_F3:
        out = K_F3;
        break;
      case SDLK_F4:
        out = K_F4;
        break;
      case SDLK_F5:
        out = K_F5;
        break;
      case SDLK_F6:
        out = K_F6;
        break;
      case SDLK_F7:
        out = K_F7;
        break;
      case SDLK_F8:
        out = K_F8;
        break;
      case SDLK_F9:
        out = K_F9;
        break;
      case SDLK_F10:
        out = K_F10;
        break;
      case SDLK_F11:
        out = K_F11;
        break;
      case SDLK_F12:
        out = K_F12;
        break;
      case SDLK_F13:
        out = K_F13;
        break;
      case SDLK_F14:
        out = K_F14;
        break;
      case SDLK_F15:
        out = K_F15;
        break;
      case SDLK_F16:
        out = K_F16;
        break;
      case SDLK_F17:
        out = K_F17;
        break;
      case SDLK_F18:
        out = K_F18;
        break;
      case SDLK_F19:
        out = K_F19;
        break;
      case SDLK_F20:
        out = K_F20;
        break;
      case SDLK_F21:
        out = K_F21;
        break;
      case SDLK_F22:
        out = K_F22;
        break;
      case SDLK_F23:
        out = K_F23;
        break;
      case SDLK_F24:
        out = K_F24;
        break;
    }
  }

  if (keysym.scancode == SDL_SCANCODE_GRAVE) {
    out = K_CONSOLE;
  }

  return out;
}

static int input_find_controller_port(struct sdl_host *host, int instance_id) {
  for (int port = 0; port < INPUT_MAX_CONTROLLERS; port++) {
    SDL_GameController *ctrl = host->controllers[port];
    SDL_Joystick *joy = SDL_GameControllerGetJoystick(ctrl);

    if (SDL_JoystickInstanceID(joy) == instance_id) {
      return port;
    }
  }

  return -1;
}

static void input_handle_mousemove(struct sdl_host *host, int port, int x,
                                   int y) {
  if (!host->input_mousemove) {
    return;
  }

  host->input_mousemove(host->userdata, port, x, y);
}

static void input_handle_keydown(struct sdl_host *host, int port,
                                 enum keycode key, int16_t value) {
  if (!host->input_keydown) {
    return;
  }

  host->input_keydown(host->userdata, port, key, value);

  /* if the key is mapped to a controller button, send that event as well */
  int button = host->key_map[key];

  if (button) {
    host->input_keydown(host->userdata, port, button, value);
  }
}

static void input_handle_controller_removed(struct sdl_host *host, int port) {
  SDL_GameController *ctrl = host->controllers[port];

  if (!ctrl) {
    return;
  }

  LOG_INFO("controller %s removed from port %d", SDL_GameControllerName(ctrl),
           port);
  SDL_GameControllerClose(ctrl);
  host->controllers[port] = NULL;
}

static void input_handle_controller_added(struct sdl_host *host,
                                          int device_id) {
  /* find the next open controller port */
  int port;
  for (port = 0; port < INPUT_MAX_CONTROLLERS; port++) {
    if (!host->controllers[port]) {
      break;
    }
  }
  if (port >= INPUT_MAX_CONTROLLERS) {
    LOG_WARNING("No open ports to bind controller to");
    return;
  }

  SDL_GameController *ctrl = SDL_GameControllerOpen(device_id);
  host->controllers[port] = ctrl;

  LOG_INFO("controller %s added on port %d", SDL_GameControllerName(ctrl),
           port);
}

static void input_shutdown(struct sdl_host *host) {
  for (int i = 0; i < INPUT_MAX_CONTROLLERS; i++) {
    input_handle_controller_removed(host, i);
  }
}

static int input_init(struct sdl_host *host) {
  /* development key map */
  host->key_map[K_SPACE] = K_CONT_START;
  host->key_map['k'] = K_CONT_A;
  host->key_map['l'] = K_CONT_B;
  host->key_map['j'] = K_CONT_X;
  host->key_map['i'] = K_CONT_Y;
  host->key_map['w'] = K_CONT_DPAD_UP;
  host->key_map['s'] = K_CONT_DPAD_DOWN;
  host->key_map['a'] = K_CONT_DPAD_LEFT;
  host->key_map['d'] = K_CONT_DPAD_RIGHT;
  host->key_map['o'] = K_CONT_LTRIG;
  host->key_map['p'] = K_CONT_RTRIG;

  /* SDL won't push events for joysticks which are already connected at init */
  int num_joysticks = SDL_NumJoysticks();

  for (int device_id = 0; device_id < num_joysticks; device_id++) {
    if (!SDL_IsGameController(device_id)) {
      continue;
    }

    input_handle_controller_added(host, device_id);
  }

  return 1;
}

void input_poll(struct host *base) {
  struct sdl_host *host = (struct sdl_host *)base;

  host_poll_events(host);
}

static void host_swap_window(struct sdl_host *host) {
  SDL_GL_SwapWindow(g_host->win);
}

static void host_poll_events(struct sdl_host *host) {
  SDL_Event ev;

  while (SDL_PollEvent(&ev)) {
    switch (ev.type) {
      case SDL_KEYDOWN: {
        enum keycode keycode = translate_sdl_key(ev.key.keysym);

        if (keycode != K_UNKNOWN) {
          input_handle_keydown(host, 0, keycode, KEY_DOWN);
        }
      } break;

      case SDL_KEYUP: {
        enum keycode keycode = translate_sdl_key(ev.key.keysym);

        if (keycode != K_UNKNOWN) {
          input_handle_keydown(host, 0, keycode, KEY_UP);
        }
      } break;

      case SDL_MOUSEBUTTONDOWN:
      case SDL_MOUSEBUTTONUP: {
        enum keycode keycode;

        switch (ev.button.button) {
          case SDL_BUTTON_LEFT:
            keycode = K_MOUSE1;
            break;
          case SDL_BUTTON_RIGHT:
            keycode = K_MOUSE2;
            break;
          case SDL_BUTTON_MIDDLE:
            keycode = K_MOUSE3;
            break;
          case SDL_BUTTON_X1:
            keycode = K_MOUSE4;
            break;
          case SDL_BUTTON_X2:
            keycode = K_MOUSE5;
            break;
          default:
            keycode = K_UNKNOWN;
            break;
        }

        if (keycode != K_UNKNOWN) {
          int16_t value = ev.type == SDL_MOUSEBUTTONDOWN ? KEY_DOWN : KEY_UP;
          input_handle_keydown(host, 0, keycode, value);
        }
      } break;

      case SDL_MOUSEWHEEL:
        if (ev.wheel.y > 0) {
          input_handle_keydown(host, 0, K_MWHEELUP, KEY_DOWN);
          input_handle_keydown(host, 0, K_MWHEELUP, KEY_UP);
        } else {
          input_handle_keydown(host, 0, K_MWHEELDOWN, KEY_DOWN);
          input_handle_keydown(host, 0, K_MWHEELDOWN, KEY_UP);
        }
        break;

      case SDL_MOUSEMOTION:
        input_handle_mousemove(host, 0, ev.motion.x, ev.motion.y);
        break;

      case SDL_CONTROLLERDEVICEADDED: {
        input_handle_controller_added(host, ev.cdevice.which);
      } break;

      case SDL_CONTROLLERDEVICEREMOVED: {
        int port = input_find_controller_port(host, ev.cdevice.which);

        if (port != -1) {
          input_handle_controller_removed(host, port);
        }
      } break;

      case SDL_CONTROLLERAXISMOTION: {
        int port = input_find_controller_port(host, ev.caxis.which);
        enum keycode key = K_UNKNOWN;
        int16_t value = ev.caxis.value;

        switch (ev.caxis.axis) {
          case SDL_CONTROLLER_AXIS_LEFTX:
            key = K_CONT_JOYX;
            break;
          case SDL_CONTROLLER_AXIS_LEFTY:
            key = K_CONT_JOYY;
            break;
          case SDL_CONTROLLER_AXIS_TRIGGERLEFT:
            key = K_CONT_LTRIG;
            break;
          case SDL_CONTROLLER_AXIS_TRIGGERRIGHT:
            key = K_CONT_RTRIG;
            break;
        }

        if (port != -1 && key != K_UNKNOWN) {
          input_handle_keydown(host, port, key, value);
        }
      } break;

      case SDL_CONTROLLERBUTTONDOWN:
      case SDL_CONTROLLERBUTTONUP: {
        int port = input_find_controller_port(host, ev.cbutton.which);
        enum keycode key = K_UNKNOWN;
        int16_t value = ev.type == SDL_CONTROLLERBUTTONDOWN ? KEY_DOWN : KEY_UP;

        switch (ev.cbutton.button) {
          case SDL_CONTROLLER_BUTTON_A:
            key = K_CONT_A;
            break;
          case SDL_CONTROLLER_BUTTON_B:
            key = K_CONT_B;
            break;
          case SDL_CONTROLLER_BUTTON_X:
            key = K_CONT_X;
            break;
          case SDL_CONTROLLER_BUTTON_Y:
            key = K_CONT_Y;
            break;
          case SDL_CONTROLLER_BUTTON_START:
            key = K_CONT_START;
            break;
          case SDL_CONTROLLER_BUTTON_DPAD_UP:
            key = K_CONT_DPAD_UP;
            break;
          case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
            key = K_CONT_DPAD_DOWN;
            break;
          case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
            key = K_CONT_DPAD_LEFT;
            break;
          case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
            key = K_CONT_DPAD_RIGHT;
            break;
        }

        if (port != -1 && key != K_UNKNOWN) {
          input_handle_keydown(host, port, key, value);
        }
      } break;

      case SDL_WINDOWEVENT:
        switch (ev.window.event) {
          case SDL_WINDOWEVENT_RESIZED: {
            host->video_width = ev.window.data1;
            host->video_height = ev.window.data2;
            video_resized(host);
          } break;
        }
        break;

      case SDL_QUIT:
        host->closed = 1;
        break;
    }
  }
}

void host_destroy(struct sdl_host *host) {
  input_shutdown(host);

  video_shutdown(host);

  audio_shutdown(host);

  if (host->win) {
    SDL_DestroyWindow(host->win);
  }

  SDL_Quit();

  free(host);
}

struct sdl_host *host_create() {
  struct sdl_host *host = calloc(1, sizeof(struct sdl_host));

  /* init sdl and create window */
  int res = SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER);
  CHECK_GE(res, 0, "SDL initialization failed: %s", SDL_GetError());

  host->video_width = VIDEO_DEFAULT_WIDTH;
  host->video_height = VIDEO_DEFAULT_HEIGHT;

  host->win = SDL_CreateWindow("redream", SDL_WINDOWPOS_UNDEFINED,
                               SDL_WINDOWPOS_UNDEFINED, host->video_width,
                               host->video_height,
                               SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
  CHECK_NOTNULL(host->win, "Window creation failed: %s", SDL_GetError());

  if (!audio_init(host)) {
    host_destroy(host);
    return NULL;
  }

  if (!video_init(host)) {
    host_destroy(host);
    return NULL;
  }

  if (!input_init(host)) {
    host_destroy(host);
    return NULL;
  }

  return host;
}

int main(int argc, char **argv) {
  const char *appdir = fs_appdir();
  if (!fs_mkdir(appdir)) {
    LOG_FATAL("Failed to create app directory %s", appdir);
  }

  /* load base options from config */
  char config[PATH_MAX] = {0};
  snprintf(config, sizeof(config), "%s" PATH_SEPARATOR "config", appdir);
  options_read(config);

  /* override options from the command line */
  options_parse(&argc, &argv);

  if (OPTION_help) {
    options_print_help();
    return EXIT_SUCCESS;
  }

  /* init host audio, video and input systems */
  g_host = host_create();
  if (!g_host) {
    return EXIT_FAILURE;
  }

  const char *load = argc > 1 ? argv[1] : NULL;

  if (load && strstr(load, ".trace")) {
    struct tracer *tracer = tracer_create((struct host *)g_host);

    if (tracer_load(tracer, load)) {
      while (!g_host->closed) {
        host_poll_events(g_host);

        tracer_run_frame(tracer);

        host_swap_window(g_host);
      }
    }

    tracer_destroy(tracer);
  } else {
    struct emu *emu = emu_create((struct host *)g_host);

    /* tell the emulator a valid video context is available */
    video_context_reset(g_host);

    if (emu_load_game(emu, load)) {
      while (!g_host->closed) {
        /* even though the emulator itself will poll for events when updating
           controller input, the main loop needs to also poll to ensure the
           close event is received */
        host_poll_events(g_host);

        /* only run a frame if the available audio is running low. this syncs
           the emulation speed with the host audio clock. note however, if
           audio is disabled, the emulator will run completely unthrottled */
        if (OPTION_audio && !audio_buffer_low(g_host)) {
          continue;
        }

        emu_run_frame(emu);

        host_swap_window(g_host);
      }
    }

    video_context_destroyed(g_host);

    emu_destroy(emu);
  }

  host_destroy(g_host);

  /* persist options for next run */
  options_write(config);

  return EXIT_SUCCESS;
}
