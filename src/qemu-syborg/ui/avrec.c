/*
 * avrec: screen + sound recorder for a QEMU phone, encoded by an ffmpeg child process into MP4
 * (H.264 + AAC, -movflags +faststart: plays on web sites as uploaded).
 *
 * The same file lives in two projects (qembian: src/qemu-syborg/ui/avrec.c, k750i: src/qemu-db2010/ui/avrec.c);
 * keep the copies identical. Docs: qembian docs/recording.md, k750i notes/recording.md.
 *
 * Use:
 *   QMP   object-add qom-type=avrec id=rec path=/tmp/x.mp4 [fps=30] [scale=1] [audiodev=snd0] [console=N]
 *         object-del id=rec                      (stops; the file is finished by ffmpeg in the background)
 *   C     avrec_toggle()                          (GUI buttons: start into $AVREC_DIR or ~/Videos, or stop)
 *   CLI   -object avrec,id=rec,path=x.mp4         (records from start-up)
 *
 * What is recorded: the picture of one QEMU console (default: the first graphic one) as the user sees it,
 * sampled at `fps` on the host clock, and the final mix of the machine's audiodev (the -machine audiodev=,
 * or `audiodev`), i.e. every sound the phone plays through it, as heard. Nothing exists while not recording.
 * While recording: a display listener (one extra copy of the console picture per refresh), an audio capture,
 * and a detached writer thread that feeds ffmpeg through two pipes (video fd 3, audio fd 4). The emulator never
 * waits for ffmpeg: frames are "latest picture" and audio sits in a bounded ring. Audio missing on the host
 * clock (machine paused, sound idle) becomes silence, so picture and sound stay in step.
 * ffmpeg runs in its own process group: Ctrl-C in QEMU's terminal does not kill it, and when QEMU exits for
 * any reason the pipes close and ffmpeg finishes the file. No ffmpeg on the host (or no libx264 in it):
 * an error on stderr, no recording; nothing else changes.
 * Environment: AVREC_FFMPEG (the ffmpeg binary), AVREC_DIR (where avrec_toggle() puts files),
 * AVREC_VCODEC (encoder, default libx264; e.g. h264_vaapi needs extra args, not supported here).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/thread.h"
#include "qemu/atomic.h"
#include "qapi/error.h"
#include "qom/object.h"
#include "qom/object_interfaces.h"
#include "ui/console.h"
#include "ui/surface.h"
#include "qemu/audio.h"
#include "qemu/audio-capture.h"
#include "system/system.h"
#include <spawn.h>
#include <poll.h>
#include <sys/wait.h>

#define TYPE_AVREC "avrec"
OBJECT_DECLARE_SIMPLE_TYPE(AvRec, AVREC)

#define AUD_RATE 48000
#define AUD_FRAME 4                         /* s16 stereo */
#define RING_BYTES (AUD_RATE * AUD_FRAME * 2) /* 2 s */
#define AUD_LAG_US 150000LL                   /* capture jitter tolerated before silence is inserted */
#define AUD_AHEAD_US 500000LL                 /* audio more than this ahead of the clock is dropped */

void avrec_toggle(void);

/* shared by the object (main thread) and the writer thread; freed by the last of the two */
typedef struct Rec {
    int refs;
    QemuMutex lock;
    bool stop, dead;
    int vfd, afd;
    pid_t pid;
    char *path;
    uint32_t fps, cw, ch;
    uint8_t *canvas;                        /* cw x ch x8r8g8b8: the latest picture */
    uint8_t *ring;                          /* audio ring */
    size_t rhead, rlen;
} Rec;

struct AvRec {
    Object parent;
    char *path, *audiodev;
    uint32_t fps, scale, console;
    Rec *rec;
    DisplayChangeListener dcl;
    pixman_image_t *canvas_img;
    bool dirty, dcl_on;
    AudioBackend *be;
    CaptureVoiceOut *cap;
};

static AvRec *avrec_active;

static void rec_unref(Rec *r)
{
    if (qatomic_fetch_dec(&r->refs) != 1) {
        return;
    }
    qemu_mutex_destroy(&r->lock);
    g_free(r->canvas);
    g_free(r->ring);
    g_free(r->path);
    g_free(r);
}

/* a write of what the pipe takes now: false when ffmpeg is gone */
static bool push(int fd, const uint8_t *p, size_t n, size_t *off)
{
    while (*off < n) {
        ssize_t k = write(fd, p + *off, n - *off);
        if (k < 0 && errno == EINTR) {
            continue;
        }
        if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return true;
        }
        if (k <= 0) {
            return false;
        }
        *off += k;
    }
    return true;
}

/* Both pipes are non-blocking and written as ffmpeg takes them: ffmpeg stops reading the input that runs ahead
   in time, so blocking on one pipe while the other starves would deadlock. Frames and samples are counted
   against the host clock when queued; a slow ffmpeg delays the file, never the sync. */
static void *writer(void *opaque)
{
    Rec *r = opaque;
    size_t fbytes = (size_t)r->cw * r->ch * 4;
    uint8_t *frame = g_malloc(fbytes);
    GByteArray *apend = g_byte_array_new();
    size_t voff = 0, aoff = 0;
    bool vbusy = false, ok = true, stop = false;
    const char *why = "";
    int err = 0;
    int64_t t0 = g_get_monotonic_time();
    uint64_t frames = 0, samples = 0;

    fcntl(r->vfd, F_SETFL, fcntl(r->vfd, F_GETFL) | O_NONBLOCK);
    fcntl(r->afd, F_SETFL, fcntl(r->afd, F_GETFL) | O_NONBLOCK);
    while (ok) {
        int64_t now = g_get_monotonic_time(), el = now - t0;
        qemu_mutex_lock(&r->lock);
        if (!stop) {
            stop = r->stop;
            /* audio: what was captured, silence up to the clock (minus jitter), drop what runs ahead */
            int64_t want = el * AUD_RATE / 1000000;
            size_t n = r->rlen;
            int64_t ahead = (int64_t)(samples + n / AUD_FRAME) - (want + AUD_AHEAD_US * AUD_RATE / 1000000);
            if (ahead > 0) {
                size_t drop = MIN(n, (size_t)ahead * AUD_FRAME);
                r->rhead = (r->rhead + drop) % RING_BYTES;
                r->rlen -= drop;
                n -= drop;
            }
            size_t at = apend->len;
            g_byte_array_set_size(apend, at + n);
            for (size_t i = 0, h = r->rhead; i < n; i++, h = (h + 1) % RING_BYTES) {
                apend->data[at + i] = r->ring[h];
            }
            r->rhead = (r->rhead + n) % RING_BYTES;
            r->rlen -= n;
            samples += n / AUD_FRAME;
            int64_t floor_s = want - (stop ? 0 : AUD_LAG_US * AUD_RATE / 1000000);   /* the end: all of it */
            if ((int64_t)samples < floor_s && apend->len - aoff < (size_t)RING_BYTES) {
                size_t z = (size_t)(floor_s - samples) * AUD_FRAME;
                at = apend->len;
                g_byte_array_set_size(apend, at + z);
                memset(apend->data + at, 0, z);
                samples += z / AUD_FRAME;
            }
        }
        /* video: the next frame (the latest picture) when the clock says one is due */
        if (!vbusy && !stop && frames < (uint64_t)(el * r->fps / 1000000)) {
            memcpy(frame, r->canvas, fbytes);
            vbusy = true;
            voff = 0;
            frames++;
        }
        qemu_mutex_unlock(&r->lock);

        bool aw = aoff < apend->len;
        if (!vbusy && !aw) {
            if (stop) {
                break;                      /* everything handed over */
            }
            int64_t next = t0 + (int64_t)((frames + 1) * 1000000 / r->fps);
            g_usleep(MAX(1000, MIN(next - g_get_monotonic_time(), 50000)));
            continue;
        }
        struct pollfd pf[2] = { { .fd = r->vfd, .events = vbusy ? POLLOUT : 0 },
                                { .fd = r->afd, .events = aw ? POLLOUT : 0 } };
        int64_t next = t0 + (int64_t)((frames + 1) * 1000000 / r->fps);
        int tmo = stop ? 200 : (int)MAX(1, MIN((next - g_get_monotonic_time()) / 1000, 50));
        int pr = poll(pf, 2, tmo);
        if (pr < 0 && errno != EINTR) {
            ok = false, why = "poll", err = errno;
        }
        if ((pf[0].revents | pf[1].revents) & (POLLERR | POLLHUP | POLLNVAL)) {
            ok = false, why = "pipe closed", err = pf[0].revents << 8 | pf[1].revents;   /* ffmpeg exited */
        }
        if (pr == 0 && stop) {              /* the end: ffmpeg takes no more (it holds off the stream that runs
                                               ahead until the other catches up): the last few ms are dropped */
            break;
        }
        if (ok && vbusy && (pf[0].revents & POLLOUT)) {
            ok = push(r->vfd, frame, fbytes, &voff);
            vbusy = voff < fbytes;
            if (!ok) {
                why = "video write", err = errno;
            }
        }
        if (ok && aw && (pf[1].revents & POLLOUT)) {
            ok = push(r->afd, apend->data, apend->len, &aoff);
            if (!ok) {
                why = "audio write", err = errno;
            }
            if (aoff == apend->len || aoff > (1u << 20)) {
                g_byte_array_remove_range(apend, 0, aoff);
                aoff = 0;
            }
        }
    }
    qemu_mutex_lock(&r->lock);
    r->dead = true;
    qemu_mutex_unlock(&r->lock);

    close(r->vfd);
    close(r->afd);
    int st = 0;
    while (waitpid(r->pid, &st, 0) < 0 && errno == EINTR) {
    }
    if (WIFEXITED(st) && WEXITSTATUS(st) == 0 && ok) {
        info_report("avrec: saved %s (%.1f s)", r->path, (double)frames / r->fps);
    } else {
        error_report("avrec: recording %s failed (%s %d; ffmpeg %s %d; see its messages above)", r->path, why, err,
                     WIFEXITED(st) ? "exit" : "signal", WIFEXITED(st) ? WEXITSTATUS(st) : WTERMSIG(st));
    }
    g_free(frame);
    g_byte_array_free(apend, true);
    rec_unref(r);
    return NULL;
}

/* ---- picture: a display listener on the console ---- */

static void compose(AvRec *s)
{
    DisplaySurface *surf = qemu_console_surface(s->dcl.con);
    Rec *r = s->rec;
    if (!surf || !surf->image) {
        return;
    }
    int sw = surface_width(surf), sh = surface_height(surf);
    qemu_mutex_lock(&r->lock);
    if ((uint32_t)sw == r->cw && (uint32_t)sh == r->ch) {
        pixman_image_composite32(PIXMAN_OP_SRC, surf->image, NULL, s->canvas_img, 0, 0, 0, 0, 0, 0, sw, sh);
    } else {                                /* size changed while recording: fit, centred, black borders */
        memset(r->canvas, 0, (size_t)r->cw * r->ch * 4);
        double f = MIN((double)r->cw / sw, (double)r->ch / sh);
        int dw = MAX(1, (int)(sw * f)), dh = MAX(1, (int)(sh * f));
        pixman_transform_t t;
        pixman_transform_init_scale(&t, pixman_double_to_fixed(1.0 / f), pixman_double_to_fixed(1.0 / f));
        pixman_image_set_transform(surf->image, &t);
        pixman_image_set_filter(surf->image, PIXMAN_FILTER_NEAREST, NULL, 0);
        pixman_image_composite32(PIXMAN_OP_SRC, surf->image, NULL, s->canvas_img, 0, 0, 0, 0,
                                 (r->cw - dw) / 2, (r->ch - dh) / 2, dw, dh);
        pixman_image_set_transform(surf->image, NULL);
    }
    qemu_mutex_unlock(&r->lock);
}

static void avrec_refresh(DisplayChangeListener *dcl)
{
    AvRec *s = container_of(dcl, AvRec, dcl);
    qemu_console_hw_update(dcl->con);       /* drives the board's update when no window does (-display none) */
    if (s->dirty) {
        s->dirty = false;
        compose(s);
    }
}

static void avrec_gfx_update(DisplayChangeListener *dcl, int x, int y, int w, int h)
{
    container_of(dcl, AvRec, dcl)->dirty = true;
}

static void avrec_gfx_switch(DisplayChangeListener *dcl, DisplaySurface *ns)
{
    container_of(dcl, AvRec, dcl)->dirty = true;
}

static const DisplayChangeListenerOps avrec_dcl_ops = {
    .dpy_name = "avrec",
    .dpy_refresh = avrec_refresh,
    .dpy_gfx_update = avrec_gfx_update,
    .dpy_gfx_switch = avrec_gfx_switch,
};

/* ---- sound: a capture of the audiodev's mix ---- */

static void cap_notify(void *opaque, audcnotification_e cmd)
{
}

static void cap_capture(void *opaque, const void *buf, int size)
{
    AvRec *s = opaque;
    Rec *r = s->rec;
    const uint8_t *p = buf;
    size -= size % AUD_FRAME;
    qemu_mutex_lock(&r->lock);
    for (int i = 0; i < size; i++) {
        size_t t = (r->rhead + r->rlen) % RING_BYTES;
        r->ring[t] = p[i];
        if (r->rlen < RING_BYTES) {
            r->rlen++;
        } else {
            r->rhead = (r->rhead + 1) % RING_BYTES;  /* writer far behind: drop the oldest */
        }
    }
    qemu_mutex_unlock(&r->lock);
}

static void cap_destroy(void *opaque)
{
    AvRec *s = opaque;
    s->cap = NULL;
}

/* ---- ffmpeg ---- */

static int high_fd(int fd)                  /* keep pipe ends off 0..4 (the child's dup2 targets) */
{
    if (fd > 4) {
        return fd;
    }
    int n = fcntl(fd, F_DUPFD_CLOEXEC, 10);
    close(fd);
    return n;
}

static bool spawn_ffmpeg(AvRec *s, Rec *r, Error **errp)
{
    int vp[2], ap[2];
    if (pipe2(vp, O_CLOEXEC) < 0) {
        error_setg_errno(errp, errno, "avrec: pipe");
        return false;
    }
    if (pipe2(ap, O_CLOEXEC) < 0) {
        error_setg_errno(errp, errno, "avrec: pipe");
        close(vp[0]);
        close(vp[1]);
        return false;
    }
    vp[0] = high_fd(vp[0]); vp[1] = high_fd(vp[1]);
    ap[0] = high_fd(ap[0]); ap[1] = high_fd(ap[1]);

    const char *bin = g_getenv("AVREC_FFMPEG") ?: "ffmpeg";
    const char *vcodec = g_getenv("AVREC_VCODEC") ?: "libx264";
    g_autofree char *size = g_strdup_printf("%ux%u", r->cw, r->ch);
    g_autofree char *rate = g_strdup_printf("%u", r->fps);
    g_autofree char *vf = g_strdup_printf("scale=iw*%u:ih*%u:flags=neighbor", s->scale, s->scale);
    g_autofree char *gop = g_strdup_printf("%u", r->fps * 2);
    GPtrArray *a = g_ptr_array_new();
    const char *fixed[] = {
        bin, "-hide_banner", "-nostdin", "-loglevel", "error", "-y",
        "-f", "rawvideo", "-pixel_format", "bgr0", "-video_size", size, "-framerate", rate, "-i", "pipe:3",
        "-f", "s16le", "-ar", "48000", "-ac", "2", "-i", "pipe:4",
        NULL };
    for (int i = 0; fixed[i]; i++) {
        g_ptr_array_add(a, (char *)fixed[i]);
    }
    if (s->scale > 1) {
        g_ptr_array_add(a, (char *)"-vf");
        g_ptr_array_add(a, vf);
    }
    const char *out[] = {
        "-c:v", vcodec, "-preset", "veryfast", "-crf", "18", "-g", gop, "-pix_fmt", "yuv420p",
        "-c:a", "aac", "-b:a", "192k", "-movflags", "+faststart", r->path, NULL };
    for (int i = 0; out[i]; i++) {
        g_ptr_array_add(a, (char *)out[i]);
    }
    g_ptr_array_add(a, NULL);

    posix_spawn_file_actions_t fa;
    posix_spawnattr_t at;
    sigset_t none;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, vp[0], 3);
    posix_spawn_file_actions_adddup2(&fa, ap[0], 4);
    posix_spawnattr_init(&at);
    sigemptyset(&none);
    posix_spawnattr_setsigmask(&at, &none);
    posix_spawnattr_setpgroup(&at, 0);      /* own process group: survives Ctrl-C in QEMU's terminal */
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK);
    int e = posix_spawnp(&r->pid, bin, &fa, &at, (char **)a->pdata, environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    g_ptr_array_free(a, true);
    close(vp[0]);
    close(ap[0]);
    if (e) {
        error_setg(errp, "avrec: cannot run %s: %s (install ffmpeg or set AVREC_FFMPEG)", bin, strerror(e));
        close(vp[1]);
        close(ap[1]);
        return false;
    }
    r->vfd = vp[1];
    r->afd = ap[1];
    return true;
}

/* ---- the object ---- */

static void avrec_complete(UserCreatable *uc, Error **errp)
{
    AvRec *s = AVREC(uc);
    QemuConsole *con;
    DisplaySurface *surf;

    if (avrec_active) {
        error_setg(errp, "avrec: already recording %s", avrec_active->rec->path);
        return;
    }
    if (!s->path || !*s->path) {
        error_setg(errp, "avrec: path= is required");
        return;
    }
    if (!s->fps || s->fps > 240 || !s->scale || s->scale > 16) {
        error_setg(errp, "avrec: fps 1..240, scale 1..16");
        return;
    }
    con = s->console == UINT32_MAX ? qemu_console_lookup_default() : qemu_console_lookup_by_index(s->console);
    surf = con ? qemu_console_surface(con) : NULL;
    if (!con || !surf) {
        error_setg(errp, "avrec: no graphic console to record");
        return;
    }

    Rec *r = g_new0(Rec, 1);
    r->refs = 1;
    r->path = g_strdup(s->path);
    r->fps = s->fps;
    r->cw = (surface_width(surf) + 1) & ~1;   /* yuv420p: even sizes */
    r->ch = (surface_height(surf) + 1) & ~1;
    r->canvas = g_malloc0((size_t)r->cw * r->ch * 4);
    r->ring = g_malloc(RING_BYTES);
    qemu_mutex_init(&r->lock);
    if (!spawn_ffmpeg(s, r, errp)) {
        rec_unref(r);
        return;
    }
    s->rec = r;
    s->canvas_img = pixman_image_create_bits(PIXMAN_x8r8g8b8, r->cw, r->ch, (uint32_t *)r->canvas, r->cw * 4);

    /* the machine's sound: audiodev=, else -machine audiodev= */
    g_autofree char *adev = NULL;
    if (s->audiodev && *s->audiodev) {
        adev = g_strdup(s->audiodev);
    } else {
        Object *m = object_resolve_path_component(object_get_root(), "machine");
        if (m && object_property_find(m, "audiodev")) {
            adev = object_property_get_str(m, "audiodev", NULL);
        }
    }
    if (adev && *adev) {
        Error *err = NULL;
        s->be = audio_be_by_name(adev, &err);
        if (s->be) {
            struct audsettings as = { .freq = AUD_RATE, .nchannels = 2, .fmt = AUDIO_FORMAT_S16,
                                      .big_endian = false };
            struct audio_capture_ops ops = { .notify = cap_notify, .capture = cap_capture,
                                             .destroy = cap_destroy };
            s->cap = audio_be_add_capture(s->be, &as, &ops, s);
        }
        if (!s->cap) {
            warn_report("avrec: no sound from audiodev %s%s%s: recording silence", adev,
                        err ? ": " : "", err ? error_get_pretty(err) : "");
        }
        error_free(err);
    }

    s->dirty = true;
    qemu_console_register_listener(con, &s->dcl, &avrec_dcl_ops);
    qemu_console_listener_set_refresh(&s->dcl, MAX(1000 / s->fps, 10));
    s->dcl_on = true;

    qatomic_inc(&r->refs);                  /* the writer's reference */
    QemuThread th;
    qemu_thread_create(&th, "avrec", writer, r, QEMU_THREAD_DETACHED);
    avrec_active = s;
    info_report("avrec: recording %ux%u @ %u fps to %s", r->cw * s->scale, r->ch * s->scale, r->fps, r->path);
}

static void avrec_finalize(Object *obj)
{
    AvRec *s = AVREC(obj);
    if (s->dcl_on) {
        qemu_console_unregister_listener(&s->dcl);
    }
    if (s->cap) {
        audio_be_del_capture(s->be, s->cap, s);
        s->cap = NULL;
    }
    if (s->rec) {                           /* the writer finishes the file in the background */
        qemu_mutex_lock(&s->rec->lock);
        s->rec->stop = true;
        qemu_mutex_unlock(&s->rec->lock);
        rec_unref(s->rec);
    }
    if (s->canvas_img) {
        pixman_image_unref(s->canvas_img);
    }
    if (avrec_active == s) {
        avrec_active = NULL;
    }
    g_free(s->path);
    g_free(s->audiodev);
}

static char *get_path(Object *o, Error **e) { return g_strdup(AVREC(o)->path); }
static void set_path(Object *o, const char *v, Error **e) { g_free(AVREC(o)->path); AVREC(o)->path = g_strdup(v); }
static char *get_adev(Object *o, Error **e) { return g_strdup(AVREC(o)->audiodev); }
static void set_adev(Object *o, const char *v, Error **e) { g_free(AVREC(o)->audiodev); AVREC(o)->audiodev = g_strdup(v); }

static bool get_rec(Object *o, Error **e)
{
    AvRec *s = AVREC(o);
    return s->rec && !qatomic_read(&s->rec->dead);
}

static void avrec_init(Object *obj)
{
    AvRec *s = AVREC(obj);
    s->fps = 30;
    s->scale = 1;
    s->console = UINT32_MAX;
    object_property_add_uint32_ptr(obj, "fps", &s->fps, OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "scale", &s->scale, OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "console", &s->console, OBJ_PROP_FLAG_READWRITE);
}

static void avrec_class_init(ObjectClass *oc, const void *data)
{
    UserCreatableClass *ucc = USER_CREATABLE_CLASS(oc);
    ucc->complete = avrec_complete;
    object_class_property_add_str(oc, "path", get_path, set_path);
    object_class_property_add_str(oc, "audiodev", get_adev, set_adev);
    object_class_property_add_bool(oc, "recording", get_rec, NULL);
}

static const TypeInfo avrec_info = {
    .name = TYPE_AVREC,
    .parent = TYPE_OBJECT,
    .instance_size = sizeof(AvRec),
    .instance_init = avrec_init,
    .instance_finalize = avrec_finalize,
    .class_init = avrec_class_init,
    .interfaces = (const InterfaceInfo[]) { { TYPE_USER_CREATABLE }, { } },
};

static void avrec_register(void)
{
    type_register_static(&avrec_info);
}
type_init(avrec_register)

/* start (a new file in $AVREC_DIR, else ~/Videos, else the current directory) or stop */
void avrec_toggle(void)
{
    if (avrec_active) {
        bool was = avrec_active->rec && !qatomic_read(&avrec_active->rec->dead);
        object_unparent(OBJECT(avrec_active));
        if (was) {
            return;
        }                                   /* it had failed: start a new one */
    }
    const char *dir = g_getenv("AVREC_DIR");
    if (!dir || !*dir) {
        dir = g_get_user_special_dir(G_USER_DIRECTORY_VIDEOS);
    }
    g_autofree char *cwd = g_get_current_dir();
    if (!dir || !g_file_test(dir, G_FILE_TEST_IS_DIR)) {
        dir = cwd;
    }
    g_autoptr(GDateTime) now = g_date_time_new_now_local();
    g_autofree char *stamp = g_date_time_format(now, "%Y%m%d-%H%M%S");
    g_autofree char *name = g_strdup_printf("%s-%s.mp4", qemu_name ? qemu_name : "qemu", stamp);
    g_autofree char *path = g_build_filename(dir, name, NULL);
    Error *err = NULL;
    Object *o = object_new_with_props(TYPE_AVREC, object_get_objects_root(), "avrec", &err,
                                      "path", path, NULL);
    if (!o) {
        error_report_err(err);
    }
}
