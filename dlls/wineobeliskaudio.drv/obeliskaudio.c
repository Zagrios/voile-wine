/*
 * Obelisk audio driver (unixlib): WASAPI render streams played, and capture streams recorded, by the Obelisk window
 * server (wsd) on macOS.
 *
 * Copyright 2011 Andrew Eikum for CodeWeavers
 *           2022 Huw Davies
 * Copyright 2026 Obelisk
 *
 * Derived from the OSS driver (dlls/wineoss.drv/oss.c): the same local buffer and timer loop, with the OSS device
 * replaced by a ring of frames shared with wsd (a memfd, struct wsd_audio_ring in wsd_protocol.h). The timer loop
 * copies the frames the application released into the ring; wsd's CoreAudio render callback consumes them and
 * publishes how far it has read, which drives the padding and the position like GETOSPACE does for OSS.
 * A capture stream is the same ring the other way round: wsd's CoreAudio input callback fills it, and the timer loop
 * copies the frames into the local buffer like the OSS driver reads its device.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#if 0
#pragma makedep unix
#endif

#include <stdarg.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "winternl.h"
#include "initguid.h"
#include "audioclient.h"
#include "mmddk.h"

#include "wine/debug.h"
#include "wine/unixlib.h"

#include "../mmdevapi/unixlib.h"
#include "wsd_protocol.h"

WINE_DEFAULT_DEBUG_CHANNEL(obeliskaudio);

struct obelisk_stream
{
    WAVEFORMATEX *fmt;
    EDataFlow flow;
    UINT flags;
    AUDCLNT_SHAREMODE share;
    HANDLE event;

    UINT32 stream_id;
    struct wsd_audio_ring *ring;
    BYTE *ring_data;
    void *map;
    SIZE_T map_len;
    UINT32 ring_frames;
    UINT64 ring_written; /* render: frames published to the ring (ring->write_frames) */
    UINT64 ring_read;    /* capture: frames taken from the ring (ring->read_frames) */

    BOOL playing, mute, please_quit;
    UINT64 written_frames, last_pos_frames;
    UINT32 period_frames, bufsize_frames, held_frames, tmp_buffer_frames, in_ring_frames;
    UINT32 lcl_offs_frames; /* offs into local_buffer where valid data starts */
    REFERENCE_TIME period;

    BYTE *local_buffer, *tmp_buffer;
    INT32 getbuf_last; /* <0 when using tmp_buffer */

    pthread_mutex_t lock;
};

static const REFERENCE_TIME def_period = 100000;
static const REFERENCE_TIME min_period = 50000;
static const char device_name[] = "wsd";
static const char device_name_input[] = "wsd-input";

static ULONG_PTR zero_bits = 0;

/* One connection to wsd per process, opened on first use. */
static pthread_mutex_t wsd_lock = PTHREAD_MUTEX_INITIALIZER;
static int wsd_fd = -1;
static UINT32 next_stream_id = 1;

static NTSTATUS obelisk_not_implemented(void *args)
{
    return STATUS_SUCCESS;
}

/* copied from kernelbase */
static int muldiv( int a, int b, int c )
{
    LONGLONG ret;

    if (!c) return -1;

    /* We want to deal with a positive divisor to simplify the logic. */
    if (c < 0)
    {
        a = -a;
        c = -c;
    }

    /* If the result is positive, we "add" to round. else, we subtract to round. */
    if ((a < 0 && b < 0) || (a >= 0 && b >= 0))
        ret = (((LONGLONG)a * b) + (c / 2)) / c;
    else
        ret = (((LONGLONG)a * b) - (c / 2)) / c;

    if (ret > 2147483647 || ret < -2147483647) return -1;
    return ret;
}

static const char *socket_path(void)
{
    const char *path = getenv("WINE_OBELISK_SOCKET");
    return path ? path : "/run/obelisk/wsd.sock";
}

static BOOL write_all(int fd, const void *data, size_t size, int fd_to_send)
{
    const char *ptr = data;
    BOOL first = TRUE;

    while (size)
    {
        struct iovec iov = { (void *)ptr, size };
        char control[CMSG_SPACE(sizeof(int))];
        struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1 };
        ssize_t ret;

        if (first && fd_to_send != -1)
        {
            struct cmsghdr *cmsg;
            memset(control, 0, sizeof(control));
            msg.msg_control = control;
            msg.msg_controllen = sizeof(control);
            cmsg = CMSG_FIRSTHDR(&msg);
            cmsg->cmsg_level = SOL_SOCKET;
            cmsg->cmsg_type = SCM_RIGHTS;
            cmsg->cmsg_len = CMSG_LEN(sizeof(int));
            memcpy(CMSG_DATA(cmsg), &fd_to_send, sizeof(int));
        }
        ret = sendmsg(fd, &msg, MSG_NOSIGNAL);
        if (ret < 0)
        {
            if (errno == EINTR) continue;
            WARN("sendmsg failed: %s\n", strerror(errno));
            return FALSE;
        }
        first = FALSE;
        ptr += ret;
        size -= ret;
    }
    return TRUE;
}

static BOOL read_all(int fd, void *data, size_t size)
{
    char *ptr = data;
    while (size)
    {
        ssize_t ret = read(fd, ptr, size);
        if (ret < 0 && errno == EINTR) continue;
        if (ret <= 0) return FALSE;
        ptr += ret;
        size -= ret;
    }
    return TRUE;
}

/* called with wsd_lock held */
static BOOL wsd_connect(void)
{
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    struct wsd_hello req = { WSD_PROTOCOL_VERSION, getpid() };
    struct { struct wsd_hdr hdr; struct wsd_hello msg; } hello = { { WSD_HELLO, sizeof(req) }, req };
    char payload[WSD_MAX_PAYLOAD];
    struct wsd_hdr hdr;
    const char *path = socket_path();
    int fd;

    if (wsd_fd != -1) return TRUE;
    if (strlen(path) >= sizeof(addr.sun_path)) return FALSE;
    strcpy(addr.sun_path, path);
    if ((fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)) < 0) return FALSE;
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || !write_all(fd, &hello, sizeof(hello), -1))
    {
        WARN("cannot connect to wsd at %s: %s\n", path, strerror(errno));
        close(fd);
        return FALSE;
    }
    /* the server answers HELLO before anything else; later replies are only errors, which nothing waits for */
    do
    {
        if (!read_all(fd, &hdr, sizeof(hdr)) || hdr.len > sizeof(payload) || !read_all(fd, payload, hdr.len))
        {
            close(fd);
            return FALSE;
        }
    } while (hdr.type != WSD_EV_HELLO);
    wsd_fd = fd;
    return TRUE;
}

static BOOL wsd_send(UINT32 type, const void *payload, UINT32 len, int fd_to_send)
{
    char buf[sizeof(struct wsd_hdr) + 64];
    struct wsd_hdr *hdr = (struct wsd_hdr *)buf;
    BOOL ret;

    if (len > sizeof(buf) - sizeof(*hdr)) return FALSE;
    hdr->type = type;
    hdr->len = len;
    memcpy(hdr + 1, payload, len);
    pthread_mutex_lock(&wsd_lock);
    ret = wsd_connect() && write_all(wsd_fd, buf, sizeof(*hdr) + len, fd_to_send);
    pthread_mutex_unlock(&wsd_lock);
    return ret;
}

/* Sends a request and waits until wsd has handled it (PING behind it, PONG back): FALSE if it answered the request
 * with an error. Other replies on this connection are only errors of earlier requests, read and dropped here. */
static BOOL wsd_request_sync(UINT32 type, const void *payload, UINT32 len, int fd_to_send)
{
    static UINT64 token;
    char buf[sizeof(struct wsd_hdr) + 64], reply[WSD_MAX_PAYLOAD];
    struct wsd_hdr *hdr = (struct wsd_hdr *)buf, in;
    struct { struct wsd_hdr hdr; struct wsd_ping ping; } ping;
    BOOL ok = FALSE, failed = FALSE;

    if (len > sizeof(buf) - sizeof(*hdr)) return FALSE;
    hdr->type = type;
    hdr->len = len;
    memcpy(hdr + 1, payload, len);
    pthread_mutex_lock(&wsd_lock);
    ping.hdr.type = WSD_PING;
    ping.hdr.len = sizeof(ping.ping);
    ping.ping.token = ++token;
    ping.ping.time_ns = 0;
    if (wsd_connect() && write_all(wsd_fd, buf, sizeof(*hdr) + len, fd_to_send) &&
        write_all(wsd_fd, &ping, sizeof(ping), -1))
    {
        for (;;)
        {
            if (!read_all(wsd_fd, &in, sizeof(in)) || in.len > sizeof(reply) || !read_all(wsd_fd, reply, in.len)) break;
            if (in.type == WSD_EV_ERROR && in.len >= sizeof(struct wsd_ev_error) &&
                ((struct wsd_ev_error *)reply)->request_type == type)
            {
                WARN("wsd refused request %u: %d\n", type, ((struct wsd_ev_error *)reply)->code);
                failed = TRUE;
            }
            if (in.type == WSD_EV_PONG && in.len >= sizeof(struct wsd_ping) &&
                ((struct wsd_ping *)reply)->token == ping.ping.token)
            {
                ok = !failed;
                break;
            }
        }
    }
    pthread_mutex_unlock(&wsd_lock);
    return ok;
}

static void obelisk_lock(struct obelisk_stream *stream)
{
    pthread_mutex_lock(&stream->lock);
}

static void obelisk_unlock(struct obelisk_stream *stream)
{
    pthread_mutex_unlock(&stream->lock);
}

static NTSTATUS obelisk_unlock_result(struct obelisk_stream *stream,
                                   HRESULT *result, HRESULT value)
{
    *result = value;
    obelisk_unlock(stream);
    return STATUS_SUCCESS;
}

static struct obelisk_stream *handle_get_stream(stream_handle h)
{
    return (struct obelisk_stream *)(UINT_PTR)h;
}

static NTSTATUS obelisk_test_connect(void *args)
{
    struct test_connect_params *params = args;
    BOOL ok;

    pthread_mutex_lock(&wsd_lock);
    ok = wsd_connect();
    pthread_mutex_unlock(&wsd_lock);
    params->priority = ok ? Priority_Preferred : Priority_Unavailable;
    return STATUS_SUCCESS;
}

static NTSTATUS obelisk_process_attach(void *args)
{
#ifdef _WIN64
    if (NtCurrentTeb()->WowTebOffset)
    {
        SYSTEM_BASIC_INFORMATION info;

        NtQuerySystemInformation(SystemEmulationBasicInformation, &info, sizeof(info), NULL);
        zero_bits = (ULONG_PTR)info.HighestUserAddress | 0x7fffffff;
    }
#endif
    return STATUS_SUCCESS;
}

static NTSTATUS obelisk_main_loop(void *args)
{
    struct main_loop_params *params = args;
    NtSetEvent(params->event, NULL);
    return STATUS_SUCCESS;
}

static NTSTATUS obelisk_get_endpoint_ids(void *args)
{
    static const WCHAR speakers[] = {'S','p','e','a','k','e','r','s',' ','(','m','a','c','O','S',')',0};
    static const WCHAR microphone[] = {'M','i','c','r','o','p','h','o','n','e',' ','(','m','a','c','O','S',')',0};
    struct get_endpoint_ids_params *params = args;
    const WCHAR *name = params->flow == eCapture ? microphone : speakers;
    const char *device = params->flow == eCapture ? device_name_input : device_name;
    SIZE_T name_size = params->flow == eCapture ? sizeof(microphone) : sizeof(speakers);
    unsigned int needed, offset;
    struct endpoint *endpoint = params->endpoints;

    params->default_idx = 0;
    if (params->flow != eRender && params->flow != eCapture)
    {
        params->num = 0;
        params->result = S_OK;
        return STATUS_SUCCESS;
    }

    offset = sizeof(*endpoint);
    needed = offset + name_size + ((strlen(device) + 2) & ~1);
    params->num = 1;
    if (needed > params->size)
    {
        params->size = needed;
        params->result = HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
        return STATUS_SUCCESS;
    }
    endpoint->name = offset;
    memcpy((char *)params->endpoints + offset, name, name_size);
    offset += name_size;
    endpoint->device = offset;
    strcpy((char *)params->endpoints + offset, device);
    params->result = S_OK;
    return STATUS_SUCCESS;
}

static UINT get_channel_mask(unsigned int channels)
{
    switch(channels){
    case 0:
        return 0;
    case 1:
        return KSAUDIO_SPEAKER_MONO;
    case 2:
        return KSAUDIO_SPEAKER_STEREO;
    case 3:
        return KSAUDIO_SPEAKER_STEREO | SPEAKER_LOW_FREQUENCY;
    case 4:
        return KSAUDIO_SPEAKER_QUAD;    /* not _SURROUND */
    case 5:
        return KSAUDIO_SPEAKER_QUAD | SPEAKER_LOW_FREQUENCY;
    case 6:
        return KSAUDIO_SPEAKER_5POINT1; /* not 5POINT1_SURROUND */
    case 7:
        return KSAUDIO_SPEAKER_5POINT1 | SPEAKER_BACK_CENTER;
    case 8:
        return KSAUDIO_SPEAKER_7POINT1_SURROUND; /* Vista deprecates 7POINT1 */
    }
    FIXME("Unknown speaker configuration: %u\n", channels);
    return 0;
}

/* WSD_AUDIO_* sample format of a WAVEFORMATEX, 0 if wsd cannot play it */
static UINT32 get_wsd_format(const WAVEFORMATEX *fmt)
{
    const WAVEFORMATEXTENSIBLE *fmtex = (const WAVEFORMATEXTENSIBLE *)fmt;

    if(fmt->wFormatTag == WAVE_FORMAT_PCM ||
            (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
             IsEqualGUID(&fmtex->SubFormat, &KSDATAFORMAT_SUBTYPE_PCM))){
        switch(fmt->wBitsPerSample){
        case 8:
            return WSD_AUDIO_U8;
        case 16:
            return WSD_AUDIO_S16;
        case 24:
            return WSD_AUDIO_S24;
        case 32:
            return WSD_AUDIO_S32;
        }
        return 0;
    }

    if(fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
            (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
             IsEqualGUID(&fmtex->SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)))
        return fmt->wBitsPerSample == 32 ? WSD_AUDIO_F32 : 0;

    return 0;
}

static WAVEFORMATEXTENSIBLE *clone_format(const WAVEFORMATEX *fmt)
{
    WAVEFORMATEXTENSIBLE *ret;
    size_t size;

    if(fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
        size = sizeof(WAVEFORMATEXTENSIBLE);
    else
        size = sizeof(WAVEFORMATEX);

    ret = malloc(size);
    if(!ret)
        return NULL;

    memcpy(ret, fmt, size);

    ret->Format.cbSize = size - sizeof(WAVEFORMATEX);

    return ret;
}

/* what setup_oss_device checks, for a device that plays any PCM layout CoreAudio can convert */
static HRESULT check_format(AUDCLNT_SHAREMODE share, const WAVEFORMATEX *fmt)
{
    const WAVEFORMATEXTENSIBLE *fmtex = (const WAVEFORMATEXTENSIBLE *)fmt;
    HRESULT ret = S_OK;

    if(!get_wsd_format(fmt))
        return AUDCLNT_E_UNSUPPORTED_FORMAT;

    if(fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
            (fmtex->Format.nAvgBytesPerSec == 0 ||
             fmtex->Format.nBlockAlign == 0 ||
             fmtex->Samples.wValidBitsPerSample > fmtex->Format.wBitsPerSample))
        return E_INVALIDARG;

    if(fmt->nChannels == 0 || fmt->nChannels > 8 || fmt->nSamplesPerSec < 4000 || fmt->nSamplesPerSec > 384000)
        return AUDCLNT_E_UNSUPPORTED_FORMAT;

    if(fmt->nBlockAlign != fmt->nChannels * fmt->wBitsPerSample / 8 ||
            fmt->nAvgBytesPerSec != fmt->nBlockAlign * fmt->nSamplesPerSec ||
            (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
             fmtex->Samples.wValidBitsPerSample < fmtex->Format.wBitsPerSample))
        ret = S_FALSE;

    if(share == AUDCLNT_SHAREMODE_EXCLUSIVE &&
            fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE){
        if(fmtex->dwChannelMask == 0 || fmtex->dwChannelMask & SPEAKER_RESERVED)
            ret = S_FALSE;
    }

    return ret;
}

/* the shared ring and its server side stream */
static HRESULT open_ring(struct obelisk_stream *stream)
{
    struct wsd_audio_open o;
    SIZE_T size;
    int fd;

    /* several periods ahead like OSS fragments, at least 100 ms */
    stream->ring_frames = max(stream->period_frames * 4, stream->fmt->nSamplesPerSec / 10);
    size = sizeof(struct wsd_audio_ring) + (SIZE_T)stream->ring_frames * stream->fmt->nBlockAlign;
    if((fd = syscall(__NR_memfd_create, "wsd-audio", 1 /* MFD_CLOEXEC */)) < 0){
        WARN("memfd_create failed: %s\n", strerror(errno));
        return E_FAIL;
    }
    if(ftruncate(fd, (size + 0xffff) & ~(SIZE_T)0xffff) < 0 || /* whole host pages for wsd's mapping */
       (stream->map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)) == MAP_FAILED){
        WARN("ring setup failed: %s\n", strerror(errno));
        stream->map = NULL;
        close(fd);
        return E_FAIL;
    }
    stream->map_len = size;
    stream->ring = stream->map;
    stream->ring_data = (BYTE *)stream->map + sizeof(struct wsd_audio_ring);

    pthread_mutex_lock(&wsd_lock);
    stream->stream_id = next_stream_id++;
    pthread_mutex_unlock(&wsd_lock);
    memset(&o, 0, sizeof(o));
    o.stream_id = stream->stream_id;
    o.format = get_wsd_format(stream->fmt);
    o.rate = stream->fmt->nSamplesPerSec;
    o.channels = stream->fmt->nChannels;
    o.frame_bytes = stream->fmt->nBlockAlign;
    o.ring_frames = stream->ring_frames;
    o.offset = 0;
    o.size = size;
    if(!wsd_request_sync(stream->flow == eCapture ? WSD_AUDIO_OPEN_INPUT : WSD_AUDIO_OPEN, &o, sizeof(o), fd)){
        close(fd);
        return AUDCLNT_E_DEVICE_INVALIDATED;
    }
    close(fd); /* wsd has its own descriptor and mapping */
    TRACE("stream %u: format %u, %u Hz, %u channels, ring %u frames\n", o.stream_id, o.format, o.rate,
          o.channels, o.ring_frames);
    return S_OK;
}

static void stream_command(struct obelisk_stream *stream, UINT32 type)
{
    struct wsd_audio_stream s = { stream->stream_id, 0 };
    if(!wsd_send(type, &s, sizeof(s), -1))
        WARN("stream %u: request %u not sent\n", stream->stream_id, type);
}

static NTSTATUS obelisk_create_stream(void *args)
{
    struct create_stream_params *params = args;
    WAVEFORMATEXTENSIBLE *fmtex;
    struct obelisk_stream *stream;
    SIZE_T size;

    if(params->flow != eRender && params->flow != eCapture){
        params->result = AUDCLNT_E_DEVICE_INVALIDATED;
        return STATUS_SUCCESS;
    }

    params->result = check_format(params->share, params->fmt);
    if(FAILED(params->result))
        return STATUS_SUCCESS;

    stream = calloc(1, sizeof(*stream));
    if(!stream){
        params->result = E_OUTOFMEMORY;
        return STATUS_SUCCESS;
    }

    stream->flow = params->flow;
    pthread_mutex_init(&stream->lock, NULL);

    fmtex = clone_format(params->fmt);
    if(!fmtex){
        params->result = E_OUTOFMEMORY;
        goto exit;
    }
    stream->fmt = &fmtex->Format;

    stream->period = params->period;
    stream->period_frames = muldiv(params->fmt->nSamplesPerSec, params->period, 10000000);

    stream->bufsize_frames = muldiv(params->duration, params->fmt->nSamplesPerSec, 10000000);
    if(params->share == AUDCLNT_SHAREMODE_EXCLUSIVE)
        stream->bufsize_frames -= stream->bufsize_frames % stream->period_frames;
    size = stream->bufsize_frames * params->fmt->nBlockAlign;
    if(NtAllocateVirtualMemory(GetCurrentProcess(), (void **)&stream->local_buffer, zero_bits,
                               &size, MEM_COMMIT, PAGE_READWRITE)){
        params->result = E_OUTOFMEMORY;
        goto exit;
    }

    stream->share = params->share;
    stream->flags = params->flags;

    if(FAILED(params->result = open_ring(stream)))
        goto exit;
    params->result = S_OK;

exit:
    if(FAILED(params->result)){
        if(stream->map) munmap(stream->map, stream->map_len);
        if(stream->local_buffer){
            size = 0;
            NtFreeVirtualMemory(GetCurrentProcess(), (void **)&stream->local_buffer, &size, MEM_RELEASE);
        }
        pthread_mutex_destroy(&stream->lock);
        free(stream->fmt);
        free(stream);
    }else{
        *params->channel_count = params->fmt->nChannels;
        *params->stream = (stream_handle)(UINT_PTR)stream;
    }

    return STATUS_SUCCESS;
}

static NTSTATUS obelisk_release_stream(void *args)
{
    struct release_stream_params *params = args;
    struct obelisk_stream *stream = handle_get_stream(params->stream);
    SIZE_T size;

    if(params->timer_thread){
        stream->please_quit = TRUE;
        NtWaitForSingleObject(params->timer_thread, FALSE, NULL);
        NtClose(params->timer_thread);
    }

    stream_command(stream, WSD_AUDIO_CLOSE);
    if(stream->map) munmap(stream->map, stream->map_len);
    if(stream->local_buffer){
        size = 0;
        NtFreeVirtualMemory(GetCurrentProcess(), (void **)&stream->local_buffer, &size, MEM_RELEASE);
    }
    if(stream->tmp_buffer){
        size = 0;
        NtFreeVirtualMemory(GetCurrentProcess(), (void **)&stream->tmp_buffer, &size, MEM_RELEASE);
    }
    free(stream->fmt);
    pthread_mutex_destroy(&stream->lock);
    free(stream);

    params->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS obelisk_start(void *args)
{
    struct start_params *params = args;
    struct obelisk_stream *stream = handle_get_stream(params->stream);

    obelisk_lock(stream);

    if((stream->flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) && !stream->event)
        return obelisk_unlock_result(stream, &params->result, AUDCLNT_E_EVENTHANDLE_NOT_SET);

    if(stream->playing)
        return obelisk_unlock_result(stream, &params->result, AUDCLNT_E_NOT_STOPPED);

    stream->playing = TRUE;
    stream_command(stream, WSD_AUDIO_START);

    return obelisk_unlock_result(stream, &params->result, S_OK);
}

static NTSTATUS obelisk_stop(void *args)
{
    struct stop_params *params = args;
    struct obelisk_stream *stream = handle_get_stream(params->stream);

    obelisk_lock(stream);

    if(!stream->playing)
        return obelisk_unlock_result(stream, &params->result, S_FALSE);

    stream->playing = FALSE;
    stream_command(stream, WSD_AUDIO_STOP); /* pause: the frames in the ring stay queued */

    return obelisk_unlock_result(stream, &params->result, S_OK);
}

static NTSTATUS obelisk_reset(void *args)
{
    struct reset_params *params = args;
    struct obelisk_stream *stream = handle_get_stream(params->stream);

    obelisk_lock(stream);

    if(stream->playing)
        return obelisk_unlock_result(stream, &params->result, AUDCLNT_E_NOT_STOPPED);

    if(stream->getbuf_last)
        return obelisk_unlock_result(stream, &params->result, AUDCLNT_E_BUFFER_OPERATION_PENDING);

    if(stream->flow == eCapture){
        /* drop what wsd recorded: we own the read position */
        stream->ring_read = atomic_load_explicit((_Atomic UINT64 *)&stream->ring->write_frames, memory_order_acquire);
        atomic_store_explicit((_Atomic UINT64 *)&stream->ring->read_frames, stream->ring_read, memory_order_release);
    }else{
        /* drop what wsd has not played: its read position jumps to our write position */
        stream_command(stream, WSD_AUDIO_RESET);
    }
    stream->written_frames = 0;
    stream->last_pos_frames = 0;
    stream->held_frames = 0;
    stream->lcl_offs_frames = 0;
    stream->in_ring_frames = 0;

    return obelisk_unlock_result(stream, &params->result, S_OK);
}

static void silence_buffer(struct obelisk_stream *stream, BYTE *buffer, UINT32 frames)
{
    WAVEFORMATEXTENSIBLE *fmtex = (WAVEFORMATEXTENSIBLE*)stream->fmt;
    if((stream->fmt->wFormatTag == WAVE_FORMAT_PCM ||
            (stream->fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
             IsEqualGUID(&fmtex->SubFormat, &KSDATAFORMAT_SUBTYPE_PCM))) &&
            stream->fmt->wBitsPerSample == 8)
        memset(buffer, 128, frames * stream->fmt->nBlockAlign);
    else
        memset(buffer, 0, frames * stream->fmt->nBlockAlign);
}

/* copy frames from the local buffer into the ring, at ring position ring_written */
static void ring_put(struct obelisk_stream *stream, const BYTE *src, UINT32 frames)
{
    UINT32 block = stream->fmt->nBlockAlign;
    while(frames){
        UINT32 pos = stream->ring_written % stream->ring_frames;
        UINT32 n = min(frames, stream->ring_frames - pos);
        if(stream->mute)
            silence_buffer(stream, stream->ring_data + (SIZE_T)pos * block, n);
        else
            memcpy(stream->ring_data + (SIZE_T)pos * block, src, (SIZE_T)n * block);
        src += (SIZE_T)n * block;
        frames -= n;
        stream->ring_written += n;
    }
}

static void obelisk_write_data(struct obelisk_stream *stream)
{
    UINT32 in_ring_frames, write_limit, write_offs_frames, new_frames, to_write_frames, space;
    UINT64 read_frames;
    SIZE_T advanced;

    read_frames = atomic_load_explicit((_Atomic UINT64 *)&stream->ring->read_frames, memory_order_acquire);
    if(read_frames > stream->ring_written) /* RESET raced with us: nothing is queued */
        read_frames = stream->ring_written;
    in_ring_frames = stream->ring_written - read_frames;

    if(in_ring_frames > stream->in_ring_frames){
        TRACE("Capping reported frames from %u to %u\n", in_ring_frames, stream->in_ring_frames);
        in_ring_frames = stream->in_ring_frames;
    }

    /* keep about three periods queued in the ring, like the OSS driver keeps in the device */
    write_limit = 0;
    while(write_limit + in_ring_frames < stream->period_frames * 3)
        write_limit += stream->period_frames;
    space = stream->ring_frames - (stream->ring_written - read_frames);
    write_limit = min(write_limit, space);

    /*        vvvvvv - in_ring_frames
     * [--xxxxxxxxxx]
     *       [xxxxxxxxxx--]
     *        ^^^^^^^^^^ - held_frames
     *        ^ - lcl_offs_frames
     */
    advanced = stream->in_ring_frames - in_ring_frames;
    if(advanced > stream->held_frames)
        advanced = stream->held_frames;
    stream->lcl_offs_frames += advanced;
    stream->lcl_offs_frames %= stream->bufsize_frames;
    stream->held_frames -= advanced;
    stream->in_ring_frames = in_ring_frames;

    if(write_limit == 0 || stream->held_frames == stream->in_ring_frames)
        return;

    write_offs_frames = (stream->lcl_offs_frames + stream->in_ring_frames) % stream->bufsize_frames;
    new_frames = min(stream->held_frames - stream->in_ring_frames, write_limit);

    to_write_frames = min(new_frames, stream->bufsize_frames - write_offs_frames);
    ring_put(stream, stream->local_buffer + (SIZE_T)write_offs_frames * stream->fmt->nBlockAlign, to_write_frames);
    if(new_frames > to_write_frames) /* wrapped: the rest is at the start of the local buffer */
        ring_put(stream, stream->local_buffer, new_frames - to_write_frames);
    stream->in_ring_frames += new_frames;

    atomic_store_explicit((_Atomic UINT64 *)&stream->ring->write_frames, stream->ring_written, memory_order_release);
}

/* copy the frames wsd recorded into the local buffer; when the application is too slow the oldest are overwritten */
static void obelisk_read_data(struct obelisk_stream *stream)
{
    UINT32 block = stream->fmt->nBlockAlign;
    UINT64 written = atomic_load_explicit((_Atomic UINT64 *)&stream->ring->write_frames, memory_order_acquire);

    while(written > stream->ring_read){
        UINT32 ring_pos = stream->ring_read % stream->ring_frames;
        UINT32 pos = (stream->lcl_offs_frames + stream->held_frames) % stream->bufsize_frames;
        UINT32 n = min(min(written - stream->ring_read, stream->ring_frames - ring_pos), stream->bufsize_frames - pos);

        memcpy(stream->local_buffer + (SIZE_T)pos * block, stream->ring_data + (SIZE_T)ring_pos * block, (SIZE_T)n * block);
        stream->ring_read += n;
        stream->held_frames += n;
        if(stream->held_frames > stream->bufsize_frames){
            WARN("Overflow of unread data\n");
            stream->lcl_offs_frames = (stream->lcl_offs_frames + stream->held_frames - stream->bufsize_frames) %
                                      stream->bufsize_frames;
            stream->held_frames = stream->bufsize_frames;
        }
    }
    atomic_store_explicit((_Atomic UINT64 *)&stream->ring->read_frames, stream->ring_read, memory_order_release);
}

static NTSTATUS obelisk_timer_loop(void *args)
{
    struct timer_loop_params *params = args;
    struct obelisk_stream *stream = handle_get_stream(params->stream);
    LARGE_INTEGER delay, now, next;
    int adjust;

    obelisk_lock(stream);

    delay.QuadPart = -stream->period;
    NtQueryPerformanceCounter(&now, NULL);
    next.QuadPart = now.QuadPart + stream->period;

    while(!stream->please_quit){
        if(stream->playing){
            if(stream->flow == eRender && stream->held_frames)
                obelisk_write_data(stream);
            else if(stream->flow == eCapture)
                obelisk_read_data(stream);
        }
        if(stream->event)
            NtSetEvent(stream->event, NULL);
        obelisk_unlock(stream);

        NtDelayExecution(FALSE, &delay);

        obelisk_lock(stream);
        NtQueryPerformanceCounter(&now, NULL);
        adjust = next.QuadPart - now.QuadPart;
        if(adjust > stream->period / 2)
            adjust = stream->period / 2;
        else if(adjust < -stream->period / 2)
            adjust = -stream->period / 2;
        delay.QuadPart = -(stream->period + adjust);
        next.QuadPart += stream->period;
    }

    obelisk_unlock(stream);

    return STATUS_SUCCESS;
}

static NTSTATUS obelisk_get_render_buffer(void *args)
{
    struct get_render_buffer_params *params = args;
    struct obelisk_stream *stream = handle_get_stream(params->stream);
    UINT32 write_pos, frames = params->frames;
    BYTE **data = params->data;
    SIZE_T size;

    obelisk_lock(stream);

    if(stream->getbuf_last)
        return obelisk_unlock_result(stream, &params->result, AUDCLNT_E_OUT_OF_ORDER);

    if(!frames)
        return obelisk_unlock_result(stream, &params->result, S_OK);

    if(stream->held_frames + frames > stream->bufsize_frames)
        return obelisk_unlock_result(stream, &params->result, AUDCLNT_E_BUFFER_TOO_LARGE);

    write_pos =
        (stream->lcl_offs_frames + stream->held_frames) % stream->bufsize_frames;
    if(write_pos + frames > stream->bufsize_frames){
        if(stream->tmp_buffer_frames < frames){
            if(stream->tmp_buffer){
                size = 0;
                NtFreeVirtualMemory(GetCurrentProcess(), (void **)&stream->tmp_buffer, &size, MEM_RELEASE);
                stream->tmp_buffer = NULL;
            }
            size = frames * stream->fmt->nBlockAlign;
            if(NtAllocateVirtualMemory(GetCurrentProcess(), (void **)&stream->tmp_buffer, zero_bits,
                                       &size, MEM_COMMIT, PAGE_READWRITE)){
                stream->tmp_buffer_frames = 0;
                return obelisk_unlock_result(stream, &params->result, E_OUTOFMEMORY);
            }
            stream->tmp_buffer_frames = frames;
        }
        *data = stream->tmp_buffer;
        stream->getbuf_last = -frames;
    }else{
        *data = stream->local_buffer + write_pos * stream->fmt->nBlockAlign;
        stream->getbuf_last = frames;
    }

    silence_buffer(stream, *data, frames);

    return obelisk_unlock_result(stream, &params->result, S_OK);
}

static void obelisk_wrap_buffer(struct obelisk_stream *stream, BYTE *buffer, UINT32 written_frames)
{
    UINT32 write_offs_frames =
        (stream->lcl_offs_frames + stream->held_frames) % stream->bufsize_frames;
    UINT32 write_offs_bytes = write_offs_frames * stream->fmt->nBlockAlign;
    UINT32 chunk_frames = stream->bufsize_frames - write_offs_frames;
    UINT32 chunk_bytes = chunk_frames * stream->fmt->nBlockAlign;
    UINT32 written_bytes = written_frames * stream->fmt->nBlockAlign;

    if(written_bytes <= chunk_bytes){
        memcpy(stream->local_buffer + write_offs_bytes, buffer, written_bytes);
    }else{
        memcpy(stream->local_buffer + write_offs_bytes, buffer, chunk_bytes);
        memcpy(stream->local_buffer, buffer + chunk_bytes,
                written_bytes - chunk_bytes);
    }
}

static NTSTATUS obelisk_release_render_buffer(void *args)
{
    struct release_render_buffer_params *params = args;
    struct obelisk_stream *stream = handle_get_stream(params->stream);
    UINT32 written_frames = params->written_frames;
    UINT flags = params->flags;
    BYTE *buffer;

    obelisk_lock(stream);

    if(!written_frames){
        stream->getbuf_last = 0;
        return obelisk_unlock_result(stream, &params->result, S_OK);
    }

    if(!stream->getbuf_last)
        return obelisk_unlock_result(stream, &params->result, AUDCLNT_E_OUT_OF_ORDER);

    if(written_frames > (stream->getbuf_last >= 0 ? stream->getbuf_last : -stream->getbuf_last))
        return obelisk_unlock_result(stream, &params->result, AUDCLNT_E_INVALID_SIZE);

    if(stream->getbuf_last >= 0)
        buffer = stream->local_buffer + stream->fmt->nBlockAlign *
          ((stream->lcl_offs_frames + stream->held_frames) % stream->bufsize_frames);
    else
        buffer = stream->tmp_buffer;

    if(flags & AUDCLNT_BUFFERFLAGS_SILENT)
        silence_buffer(stream, buffer, written_frames);

    if(stream->getbuf_last < 0)
        obelisk_wrap_buffer(stream, buffer, written_frames);

    stream->held_frames += written_frames;
    stream->written_frames += written_frames;
    stream->getbuf_last = 0;

    /* Hand the frames to wsd now, not at the next timer tick: until then the ring held about one period less than
     * the client had written, and wsd ran short several times a second (crackles). */
    if(stream->playing)
        obelisk_write_data(stream);

    return obelisk_unlock_result(stream, &params->result, S_OK);
}

static NTSTATUS obelisk_get_capture_buffer(void *args)
{
    struct get_capture_buffer_params *params = args;
    struct obelisk_stream *stream = handle_get_stream(params->stream);
    UINT64 *devpos = params->devpos, *qpcpos = params->qpcpos;
    UINT32 *frames = params->frames;
    UINT *flags = params->flags;
    BYTE **data = params->data;
    SIZE_T size;

    obelisk_lock(stream);

    if(stream->getbuf_last)
        return obelisk_unlock_result(stream, &params->result, AUDCLNT_E_OUT_OF_ORDER);

    if(stream->held_frames < stream->period_frames){
        *frames = 0;
        return obelisk_unlock_result(stream, &params->result, AUDCLNT_S_BUFFER_EMPTY);
    }

    *flags = 0;

    *frames = stream->period_frames;

    if(stream->lcl_offs_frames + *frames > stream->bufsize_frames){
        UINT32 chunk_bytes, offs_bytes, frames_bytes;
        if(stream->tmp_buffer_frames < *frames){
            if(stream->tmp_buffer){
                size = 0;
                NtFreeVirtualMemory(GetCurrentProcess(), (void **)&stream->tmp_buffer, &size, MEM_RELEASE);
                stream->tmp_buffer = NULL;
            }
            size = *frames * stream->fmt->nBlockAlign;
            if(NtAllocateVirtualMemory(GetCurrentProcess(), (void **)&stream->tmp_buffer, zero_bits,
                                       &size, MEM_COMMIT, PAGE_READWRITE)){
                stream->tmp_buffer_frames = 0;
                return obelisk_unlock_result(stream, &params->result, E_OUTOFMEMORY);
            }
            stream->tmp_buffer_frames = *frames;
        }

        *data = stream->tmp_buffer;
        chunk_bytes = (stream->bufsize_frames - stream->lcl_offs_frames) * stream->fmt->nBlockAlign;
        offs_bytes = stream->lcl_offs_frames * stream->fmt->nBlockAlign;
        frames_bytes = *frames * stream->fmt->nBlockAlign;
        memcpy(stream->tmp_buffer, stream->local_buffer + offs_bytes, chunk_bytes);
        memcpy(stream->tmp_buffer + chunk_bytes, stream->local_buffer, frames_bytes - chunk_bytes);
    }else
        *data = stream->local_buffer + stream->lcl_offs_frames * stream->fmt->nBlockAlign;

    stream->getbuf_last = *frames;

    if(devpos)
        *devpos = stream->written_frames;
    if(qpcpos){
        LARGE_INTEGER stamp, freq;
        NtQueryPerformanceCounter(&stamp, &freq);
        *qpcpos = (stamp.QuadPart * (INT64)10000000) / freq.QuadPart;
    }

    return obelisk_unlock_result(stream, &params->result, S_OK);
}

static NTSTATUS obelisk_release_capture_buffer(void *args)
{
    struct release_capture_buffer_params *params = args;
    struct obelisk_stream *stream = handle_get_stream(params->stream);
    UINT32 done = params->done;

    obelisk_lock(stream);

    if(!done){
        stream->getbuf_last = 0;
        return obelisk_unlock_result(stream, &params->result, S_OK);
    }

    if(!stream->getbuf_last)
        return obelisk_unlock_result(stream, &params->result, AUDCLNT_E_OUT_OF_ORDER);

    if(stream->getbuf_last != done)
        return obelisk_unlock_result(stream, &params->result, AUDCLNT_E_INVALID_SIZE);

    stream->written_frames += done;
    stream->held_frames -= done;
    stream->lcl_offs_frames += done;
    stream->lcl_offs_frames %= stream->bufsize_frames;
    stream->getbuf_last = 0;

    return obelisk_unlock_result(stream, &params->result, S_OK);
}

static NTSTATUS obelisk_is_format_supported(void *args)
{
    struct is_format_supported_params *params = args;

    params->result = params->flow == eRender || params->flow == eCapture ? check_format(params->share, params->fmt_in)
                                                                         : AUDCLNT_E_DEVICE_INVALIDATED;
    return STATUS_SUCCESS;
}

static NTSTATUS obelisk_get_mix_format(void *args)
{
    struct get_mix_format_params *params = args;
    WAVEFORMATEXTENSIBLE *fmt = params->fmt;

    if(params->flow != eRender && params->flow != eCapture){
        params->result = E_UNEXPECTED;
        return STATUS_SUCCESS;
    }

    /* what a Mac (and most Windows machines) mix in: 32-bit float stereo at 48 kHz; a microphone is mono, and wsd
     * converts whatever its device records to the format the stream asks for */
    fmt->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    fmt->Format.wBitsPerSample = 32;
    fmt->SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    fmt->Format.nChannels = params->flow == eCapture ? 1 : 2;
    fmt->Format.nSamplesPerSec = 48000;
    fmt->dwChannelMask = get_channel_mask(fmt->Format.nChannels);
    fmt->Format.nBlockAlign = (fmt->Format.wBitsPerSample * fmt->Format.nChannels) / 8;
    fmt->Format.nAvgBytesPerSec = fmt->Format.nSamplesPerSec * fmt->Format.nBlockAlign;
    fmt->Samples.wValidBitsPerSample = fmt->Format.wBitsPerSample;
    fmt->Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);

    params->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS obelisk_get_device_period(void *args)
{
    struct get_device_period_params *params = args;

    if (params->def_period)
        *params->def_period = def_period;
    if (params->min_period)
        *params->min_period = min_period;

    params->result = S_OK;

    return STATUS_SUCCESS;
}

static NTSTATUS obelisk_get_buffer_size(void *args)
{
    struct get_buffer_size_params *params = args;
    struct obelisk_stream *stream = handle_get_stream(params->stream);

    obelisk_lock(stream);

    *params->frames = stream->bufsize_frames;

    return obelisk_unlock_result(stream, &params->result, S_OK);
}

static NTSTATUS obelisk_get_latency(void *args)
{
    struct get_latency_params *params = args;
    struct obelisk_stream *stream = handle_get_stream(params->stream);

    obelisk_lock(stream);

    /* pretend we process audio in Period chunks, so max latency includes
     * the period time.  Some native machines add .6666ms in shared mode. */
    *params->latency = stream->period + 6666;

    return obelisk_unlock_result(stream, &params->result, S_OK);
}

static NTSTATUS obelisk_get_current_padding(void *args)
{
    struct get_current_padding_params *params = args;
    struct obelisk_stream *stream = handle_get_stream(params->stream);

    obelisk_lock(stream);

    *params->padding = stream->held_frames;

    return obelisk_unlock_result(stream, &params->result, S_OK);
}

static NTSTATUS obelisk_get_next_packet_size(void *args)
{
    struct get_next_packet_size_params *params = args;
    struct obelisk_stream *stream = handle_get_stream(params->stream);
    UINT32 *frames = params->frames;

    obelisk_lock(stream);

    *frames = stream->held_frames < stream->period_frames ? 0 : stream->period_frames;

    return obelisk_unlock_result(stream, &params->result, S_OK);
}

static NTSTATUS obelisk_get_frequency(void *args)
{
    struct get_frequency_params *params = args;
    struct obelisk_stream *stream = handle_get_stream(params->stream);
    UINT64 *freq = params->freq;

    obelisk_lock(stream);

    if(stream->share == AUDCLNT_SHAREMODE_SHARED)
        *freq = (UINT64)stream->fmt->nSamplesPerSec * stream->fmt->nBlockAlign;
    else
        *freq = stream->fmt->nSamplesPerSec;

    return obelisk_unlock_result(stream, &params->result, S_OK);
}

static NTSTATUS obelisk_get_position(void *args)
{
    struct get_position_params *params = args;
    struct obelisk_stream *stream = handle_get_stream(params->stream);
    UINT64 *pos = params->pos, *qpctime = params->qpctime;

    if (params->device) {
        FIXME("Device position reporting not implemented\n");
        params->result = E_NOTIMPL;
        return STATUS_SUCCESS;
    }

    obelisk_lock(stream);

    if(stream->flow == eCapture){
        /* what the application took plus what waits in the local buffer */
        *pos = stream->written_frames + stream->held_frames;
    }else{
        /* held_frames still counts what wsd has not read from the ring: the difference is what was played */
        *pos = stream->written_frames - stream->held_frames;
        if(*pos < stream->last_pos_frames)
            *pos = stream->last_pos_frames;
    }

    stream->last_pos_frames = *pos;

    TRACE("returning: %s\n", wine_dbgstr_longlong(*pos));
    if(stream->share == AUDCLNT_SHAREMODE_SHARED)
        *pos *= stream->fmt->nBlockAlign;

    if(qpctime){
        LARGE_INTEGER stamp, freq;
        NtQueryPerformanceCounter(&stamp, &freq);
        *qpctime = (stamp.QuadPart * (INT64)10000000) / freq.QuadPart;
    }

    return obelisk_unlock_result(stream, &params->result, S_OK);
}

static NTSTATUS obelisk_set_volumes(void *args)
{
    struct set_volumes_params *params = args;
    struct obelisk_stream *stream = handle_get_stream(params->stream);
    UINT16 i;

    if (params->master_volume) {
        for (i = 0; i < stream->fmt->nChannels; ++i) {
            if (params->master_volume * params->volumes[i] * params->session_volumes[i] != 1.0f) {
                FIXME("Volume control is not implemented\n");
                break;
            }
        }
    }

    obelisk_lock(stream);
    stream->mute = !params->master_volume;
    obelisk_unlock(stream);

    return STATUS_SUCCESS;
}

static NTSTATUS obelisk_set_event_handle(void *args)
{
    struct set_event_handle_params *params = args;
    struct obelisk_stream *stream = handle_get_stream(params->stream);

    obelisk_lock(stream);

    if(!(stream->flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK))
        return obelisk_unlock_result(stream, &params->result, AUDCLNT_E_EVENTHANDLE_NOT_EXPECTED);

    if (stream->event){
        FIXME("called twice\n");
        return obelisk_unlock_result(stream, &params->result, HRESULT_FROM_WIN32(ERROR_INVALID_NAME));
    }

    stream->event = params->event;

    return obelisk_unlock_result(stream, &params->result, S_OK);
}

static NTSTATUS obelisk_is_started(void *args)
{
    struct is_started_params *params = args;
    struct obelisk_stream *stream = handle_get_stream(params->stream);

    obelisk_lock(stream);

    return obelisk_unlock_result(stream, &params->result, stream->playing ? S_OK : S_FALSE);
}

static NTSTATUS obelisk_get_prop_value(void *args)
{
    struct get_prop_value_params *params = args;

    params->result = E_NOTIMPL;

    return STATUS_SUCCESS;
}

/* No MIDI and no aux (mixer) devices: mmdevapi uses the selected audio driver for MIDI too. */
static NTSTATUS obelisk_midi_init(void *args)
{
    struct midi_init_params *params = args;
    *params->err = MMSYSERR_ERROR; /* DRV_LOAD fails: no MIDI notification thread */
    return STATUS_SUCCESS;
}

static NTSTATUS obelisk_midi_out_message(void *args)
{
    struct midi_out_message_params *params = args;
    if (params->notify) params->notify->send_notify = FALSE;
    *params->err = params->msg == MODM_GETNUMDEVS ? 0 : MMSYSERR_BADDEVICEID;
    return STATUS_SUCCESS;
}

static NTSTATUS obelisk_midi_in_message(void *args)
{
    struct midi_in_message_params *params = args;
    if (params->notify) params->notify->send_notify = FALSE;
    *params->err = params->msg == MIDM_GETNUMDEVS ? 0 : MMSYSERR_BADDEVICEID;
    return STATUS_SUCCESS;
}

static NTSTATUS obelisk_midi_notify_wait(void *args)
{
    struct midi_notify_wait_params *params = args;
    *params->quit = TRUE;
    return STATUS_SUCCESS;
}

static NTSTATUS obelisk_aux_message(void *args)
{
    struct aux_message_params *params = args;
    *params->err = params->msg == AUXDM_GETNUMDEVS ? 0 : MMSYSERR_BADDEVICEID;
    return STATUS_SUCCESS;
}

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    obelisk_process_attach,
    obelisk_not_implemented,
    obelisk_main_loop,
    obelisk_get_endpoint_ids,
    obelisk_create_stream,
    obelisk_release_stream,
    obelisk_start,
    obelisk_stop,
    obelisk_reset,
    obelisk_timer_loop,
    obelisk_get_render_buffer,
    obelisk_release_render_buffer,
    obelisk_get_capture_buffer,
    obelisk_release_capture_buffer,
    obelisk_is_format_supported,
    obelisk_not_implemented,
    obelisk_get_mix_format,
    obelisk_get_device_period,
    obelisk_get_buffer_size,
    obelisk_get_latency,
    obelisk_get_current_padding,
    obelisk_get_next_packet_size,
    obelisk_get_frequency,
    obelisk_get_position,
    obelisk_set_volumes,
    obelisk_set_event_handle,
    obelisk_not_implemented,
    obelisk_test_connect,
    obelisk_is_started,
    obelisk_get_prop_value,
    obelisk_not_implemented,
    obelisk_midi_init,
    obelisk_not_implemented,
    obelisk_midi_out_message,
    obelisk_midi_in_message,
    obelisk_midi_notify_wait,
    obelisk_aux_message,
};

C_ASSERT(ARRAYSIZE(__wine_unix_call_funcs) == funcs_count);
