/*
 * Buffered I/O for ffmpeg system
 * Copyright (c) 2000,2001 Fabrice Bellard
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "libavutil/crc.h"
#include "libavutil/intreadwrite.h"
#include "avformat.h"
#include "avio.h"
#include "avio_internal.h"
#include "internal.h"
#include <stdarg.h>

#define IO_BUFFER_SIZE 32768

/**
 * Do seeks within this distance ahead of the current buffer by skipping
 * data instead of calling the protocol seek function, for seekable
 * protocols.
 */
#define SHORT_SEEK_THRESHOLD 4096

static void fill_buffer(AVIOContext *s);
#if !FF_API_URL_RESETBUF
static int url_resetbuf(AVIOContext *s, int flags);
#endif

int ffio_init_context(AVIOContext *s,
                  unsigned char *buffer,
                  int buffer_size,
                  int write_flag,
                  void *opaque,
                  int (*read_packet)(void *opaque, uint8_t *buf, int buf_size),
                  int (*write_packet)(void *opaque, uint8_t *buf, int buf_size),
                  int64_t (*seek)(void *opaque, int64_t offset, int whence))
{
    s->buffer = buffer;
    s->buffer_size = buffer_size;
    s->buf_ptr = buffer;
    s->opaque = opaque;
    url_resetbuf(s, write_flag ? URL_WRONLY : URL_RDONLY);
    s->write_packet = write_packet;
    s->read_packet = read_packet;
    s->seek = seek;
    s->pos = 0;
    s->must_flush = 0;
    s->eof_reached = 0;
    s->error = 0;
    s->is_streamed = 0;
    s->max_packet_size = 0;
    s->update_checksum= NULL;
    if(!read_packet && !write_flag){
        s->pos = buffer_size;
        s->buf_end = s->buffer + buffer_size;
    }
    s->read_pause = NULL;
    s->read_seek  = NULL;
    return 0;
}

#if FF_API_OLD_AVIO
int init_put_byte(AVIOContext *s,
                  unsigned char *buffer,
                  int buffer_size,
                  int write_flag,
                  void *opaque,
                  int (*read_packet)(void *opaque, uint8_t *buf, int buf_size),
                  int (*write_packet)(void *opaque, uint8_t *buf, int buf_size),
                  int64_t (*seek)(void *opaque, int64_t offset, int whence))
{
    return ffio_init_context(s, buffer, buffer_size, write_flag, opaque,
                                read_packet, write_packet, seek);
}
AVIOContext *av_alloc_put_byte(
                  unsigned char *buffer,
                  int buffer_size,
                  int write_flag,
                  void *opaque,
                  int (*read_packet)(void *opaque, uint8_t *buf, int buf_size),
                  int (*write_packet)(void *opaque, uint8_t *buf, int buf_size),
                  int64_t (*seek)(void *opaque, int64_t offset, int whence))
{
    return avio_alloc_context(buffer, buffer_size, write_flag, opaque,
                              read_packet, write_packet, seek);
}
#endif

AVIOContext *avio_alloc_context(
                  unsigned char *buffer,
                  int buffer_size,
                  int write_flag,
                  void *opaque,
                  int (*read_packet)(void *opaque, uint8_t *buf, int buf_size),
                  int (*write_packet)(void *opaque, uint8_t *buf, int buf_size),
                  int64_t (*seek)(void *opaque, int64_t offset, int whence))
{
    AVIOContext *s = av_mallocz(sizeof(AVIOContext));
    ffio_init_context(s, buffer, buffer_size, write_flag, opaque,
                  read_packet, write_packet, seek);
    return s;
}

static void flush_buffer(AVIOContext *s)
{
    if (s->buf_ptr > s->buffer) {
        if (s->write_packet && !s->error){
            int ret= s->write_packet(s->opaque, s->buffer, s->buf_ptr - s->buffer);
            if(ret < 0){
                s->error = ret;
            }
        }
        if(s->update_checksum){
            s->checksum= s->update_checksum(s->checksum, s->checksum_ptr, s->buf_ptr - s->checksum_ptr);
            s->checksum_ptr= s->buffer;
        }
        s->pos += s->buf_ptr - s->buffer;
    }
    s->buf_ptr = s->buffer;
}

void put_byte(AVIOContext *s, int b)
{
    *(s->buf_ptr)++ = b;
    if (s->buf_ptr >= s->buf_end)
        flush_buffer(s);
}

void put_nbyte(AVIOContext *s, int b, int count)
{
    while (count > 0) {
        int len = FFMIN(s->buf_end - s->buf_ptr, count);
        memset(s->buf_ptr, b, len);
        s->buf_ptr += len;

        if (s->buf_ptr >= s->buf_end)
            flush_buffer(s);

        count -= len;
    }
}

void put_buffer(AVIOContext *s, const unsigned char *buf, int size)
{
    while (size > 0) {
        int len = FFMIN(s->buf_end - s->buf_ptr, size);
        memcpy(s->buf_ptr, buf, len);
        s->buf_ptr += len;

        if (s->buf_ptr >= s->buf_end)
            flush_buffer(s);

        buf += len;
        size -= len;
    }
}

void put_flush_packet(AVIOContext *s)
{
    flush_buffer(s);
    s->must_flush = 0;
}

int64_t url_fseek(AVIOContext *s, int64_t offset, int whence)
{
    int64_t offset1;
    int64_t pos;
    int force = whence & AVSEEK_FORCE;
    whence &= ~AVSEEK_FORCE;

    if(!s)
        return AVERROR(EINVAL);

    pos = s->pos - (s->write_flag ? 0 : (s->buf_end - s->buffer));

    if (whence != SEEK_CUR && whence != SEEK_SET)
        return AVERROR(EINVAL);

    if (whence == SEEK_CUR) {
        offset1 = pos + (s->buf_ptr - s->buffer);
        if (offset == 0)
            return offset1;
        offset += offset1;
    }
    offset1 = offset - pos;
    if (!s->must_flush &&
        offset1 >= 0 && offset1 <= (s->buf_end - s->buffer)) {
        /* can do the seek inside the buffer */
        s->buf_ptr = s->buffer + offset1;
    } else if ((s->is_streamed ||
               offset1 <= s->buf_end + SHORT_SEEK_THRESHOLD - s->buffer) &&
               !s->write_flag && offset1 >= 0 &&
              (whence != SEEK_END || force)) {
        while(s->pos < offset && !s->eof_reached)
            fill_buffer(s);
        if (s->eof_reached)
            return AVERROR_EOF;
        s->buf_ptr = s->buf_end + offset - s->pos;
    } else {
        int64_t res;

#if CONFIG_MUXERS || CONFIG_NETWORK
        if (s->write_flag) {
            flush_buffer(s);
            s->must_flush = 1;
        }
#endif /* CONFIG_MUXERS || CONFIG_NETWORK */
        if (!s->seek)
            return AVERROR(EPIPE);
        if ((res = s->seek(s->opaque, offset, SEEK_SET)) < 0)
            return res;
        if (!s->write_flag)
            s->buf_end = s->buffer;
        s->buf_ptr = s->buffer;
        s->pos = offset;
    }
    s->eof_reached = 0;
    return offset;
}

int url_fskip(AVIOContext *s, int64_t offset)
{
    int64_t ret = url_fseek(s, offset, SEEK_CUR);
    return ret < 0 ? ret : 0;
}

int64_t url_ftell(AVIOContext *s)
{
    return url_fseek(s, 0, SEEK_CUR);
}

int64_t url_fsize(AVIOContext *s)
{
    int64_t size;

    if(!s)
        return AVERROR(EINVAL);

    if (!s->seek)
        return AVERROR(ENOSYS);
    size = s->seek(s->opaque, 0, AVSEEK_SIZE);
    if(size<0){
        if ((size = s->seek(s->opaque, -1, SEEK_END)) < 0)
            return size;
        size++;
        s->seek(s->opaque, s->pos, SEEK_SET);
    }
    return size;
}

int url_feof(AVIOContext *s)
{
    if(!s)
        return 0;
    return s->eof_reached;
}

int url_ferror(AVIOContext *s)
{
    if(!s)
        return 0;
    return s->error;
}

void put_le32(AVIOContext *s, unsigned int val)
{
    put_byte(s, val);
    put_byte(s, val >> 8);
    put_byte(s, val >> 16);
    put_byte(s, val >> 24);
}

void put_be32(AVIOContext *s, unsigned int val)
{
    put_byte(s, val >> 24);
    put_byte(s, val >> 16);
    put_byte(s, val >> 8);
    put_byte(s, val);
}

#if FF_API_OLD_AVIO
void put_strz(AVIOContext *s, const char *str)
{
    avio_put_str(s, str);
}

#define GET(name, type) \
    type get_be ##name(AVIOContext *s) \
{\
    return avio_rb ##name(s);\
}\
    type get_le ##name(AVIOContext *s) \
{\
    return avio_rl ##name(s);\
}

GET(16, unsigned int)
GET(24, unsigned int)
GET(32, unsigned int)
GET(64, uint64_t)

#undef GET

int get_byte(AVIOContext *s)
{
   return avio_r8(s);
}
int get_buffer(AVIOContext *s, unsigned char *buf, int size)
{
    return avio_read(s, buf, size);
}
#endif

int avio_put_str(AVIOContext *s, const char *str)
{
    int len = 1;
    if (str) {
        len += strlen(str);
        put_buffer(s, (const unsigned char *) str, len);
    } else
        put_byte(s, 0);
    return len;
}

int avio_put_str16le(AVIOContext *s, const char *str)
{
    const uint8_t *q = str;
    int ret = 0;

    while (*q) {
        uint32_t ch;
        uint16_t tmp;

        GET_UTF8(ch, *q++, break;)
        PUT_UTF16(ch, tmp, put_le16(s, tmp);ret += 2;)
    }
    put_le16(s, 0);
    ret += 2;
    return ret;
}

int ff_get_v_length(uint64_t val){
    int i=1;

    while(val>>=7)
        i++;

    return i;
}

void ff_put_v(AVIOContext *bc, uint64_t val){
    int i= ff_get_v_length(val);

    while(--i>0)
        put_byte(bc, 128 | (val>>(7*i)));

    put_byte(bc, val&127);
}

void put_le64(AVIOContext *s, uint64_t val)
{
    put_le32(s, (uint32_t)(val & 0xffffffff));
    put_le32(s, (uint32_t)(val >> 32));
}

void put_be64(AVIOContext *s, uint64_t val)
{
    put_be32(s, (uint32_t)(val >> 32));
    put_be32(s, (uint32_t)(val & 0xffffffff));
}

void put_le16(AVIOContext *s, unsigned int val)
{
    put_byte(s, val);
    put_byte(s, val >> 8);
}

void put_be16(AVIOContext *s, unsigned int val)
{
    put_byte(s, val >> 8);
    put_byte(s, val);
}

void put_le24(AVIOContext *s, unsigned int val)
{
    put_le16(s, val & 0xffff);
    put_byte(s, val >> 16);
}

void put_be24(AVIOContext *s, unsigned int val)
{
    put_be16(s, val >> 8);
    put_byte(s, val);
}

void put_tag(AVIOContext *s, const char *tag)
{
    while (*tag) {
        put_byte(s, *tag++);
    }
}

/* Input stream */

static void fill_buffer(AVIOContext *s)
{
    uint8_t *dst= !s->max_packet_size && s->buf_end - s->buffer < s->buffer_size ? s->buf_ptr : s->buffer;
    int len= s->buffer_size - (dst - s->buffer);
    int max_buffer_size = s->max_packet_size ? s->max_packet_size : IO_BUFFER_SIZE;

    /* no need to do anything if EOF already reached */
    if (s->eof_reached)
        return;

    if(s->update_checksum && dst == s->buffer){
        if(s->buf_end > s->checksum_ptr)
            s->checksum= s->update_checksum(s->checksum, s->checksum_ptr, s->buf_end - s->checksum_ptr);
        s->checksum_ptr= s->buffer;
    }

    /* make buffer smaller in case it ended up large after probing */
    if (s->buffer_size > max_buffer_size) {
        url_setbufsize(s, max_buffer_size);

        s->checksum_ptr = dst = s->buffer;
        len = s->buffer_size;
    }

    if(s->read_packet)
        len = s->read_packet(s->opaque, dst, len);
    else
        len = 0;
    if (len <= 0) {
        /* do not modify buffer if EOF reached so that a seek back can
           be done without rereading data */
        s->eof_reached = 1;
        if(len<0)
            s->error= len;
    } else {
        s->pos += len;
        s->buf_ptr = dst;
        s->buf_end = dst + len;
    }
}

unsigned long ff_crc04C11DB7_update(unsigned long checksum, const uint8_t *buf,
                                    unsigned int len)
{
    return av_crc(av_crc_get_table(AV_CRC_32_IEEE), checksum, buf, len);
}

unsigned long get_checksum(AVIOContext *s)
{
    s->checksum= s->update_checksum(s->checksum, s->checksum_ptr, s->buf_ptr - s->checksum_ptr);
    s->update_checksum= NULL;
    return s->checksum;
}

void init_checksum(AVIOContext *s,
                   unsigned long (*update_checksum)(unsigned long c, const uint8_t *p, unsigned int len),
                   unsigned long checksum)
{
    s->update_checksum= update_checksum;
    if(s->update_checksum){
        s->checksum= checksum;
        s->checksum_ptr= s->buf_ptr;
    }
}

/* XXX: put an inline version */
int avio_r8(AVIOContext *s)
{
    if (s->buf_ptr >= s->buf_end)
        fill_buffer(s);
    if (s->buf_ptr < s->buf_end)
        return *s->buf_ptr++;
    return 0;
}

int url_fgetc(AVIOContext *s)
{
    if (s->buf_ptr >= s->buf_end)
        fill_buffer(s);
    if (s->buf_ptr < s->buf_end)
        return *s->buf_ptr++;
    return URL_EOF;
}

int avio_read(AVIOContext *s, unsigned char *buf, int size)
{
    int len, size1;

    size1 = size;
    while (size > 0) {
        len = s->buf_end - s->buf_ptr;
        if (len > size)
            len = size;
        if (len == 0) {
            if(size > s->buffer_size && !s->update_checksum){
                if(s->read_packet)
                    len = s->read_packet(s->opaque, buf, size);
                if (len <= 0) {
                    /* do not modify buffer if EOF reached so that a seek back can
                    be done without rereading data */
                    s->eof_reached = 1;
                    if(len<0)
                        s->error= len;
                    break;
                } else {
                    s->pos += len;
                    size -= len;
                    buf += len;
                    s->buf_ptr = s->buffer;
                    s->buf_end = s->buffer/* + len*/;
                }
            }else{
                fill_buffer(s);
                len = s->buf_end - s->buf_ptr;
                if (len == 0)
                    break;
            }
        } else {
            memcpy(buf, s->buf_ptr, len);
            buf += len;
            s->buf_ptr += len;
            size -= len;
        }
    }
    if (size1 == size) {
        if (url_ferror(s)) return url_ferror(s);
        if (url_feof(s))   return AVERROR_EOF;
    }
    return size1 - size;
}

int get_partial_buffer(AVIOContext *s, unsigned char *buf, int size)
{
    int len;

    if(size<0)
        return -1;

    len = s->buf_end - s->buf_ptr;
    if (len == 0) {
        fill_buffer(s);
        len = s->buf_end - s->buf_ptr;
    }
    if (len > size)
        len = size;
    memcpy(buf, s->buf_ptr, len);
    s->buf_ptr += len;
    if (!len) {
        if (url_ferror(s)) return url_ferror(s);
        if (url_feof(s))   return AVERROR_EOF;
    }
    return len;
}

unsigned int avio_rl16(AVIOContext *s)
{
    unsigned int val;
    val = avio_r8(s);
    val |= avio_r8(s) << 8;
    return val;
}

unsigned int avio_rl24(AVIOContext *s)
{
    unsigned int val;
    val = avio_rl16(s);
    val |= avio_r8(s) << 16;
    return val;
}

unsigned int avio_rl32(AVIOContext *s)
{
    unsigned int val;
    val = avio_rl16(s);
    val |= avio_rl16(s) << 16;
    return val;
}

uint64_t avio_rl64(AVIOContext *s)
{
    uint64_t val;
    val = (uint64_t)avio_rl32(s);
    val |= (uint64_t)avio_rl32(s) << 32;
    return val;
}

unsigned int avio_rb16(AVIOContext *s)
{
    unsigned int val;
    val = avio_r8(s) << 8;
    val |= avio_r8(s);
    return val;
}

unsigned int avio_rb24(AVIOContext *s)
{
    unsigned int val;
    val = avio_rb16(s) << 8;
    val |= avio_r8(s);
    return val;
}
unsigned int avio_rb32(AVIOContext *s)
{
    unsigned int val;
    val = avio_rb16(s) << 16;
    val |= avio_rb16(s);
    return val;
}

char *get_strz(AVIOContext *s, char *buf, int maxlen)
{
    int i = 0;
    char c;

    while ((c = avio_r8(s))) {
        if (i < maxlen-1)
            buf[i++] = c;
    }

    buf[i] = 0; /* Ensure null terminated, but may be truncated */

    return buf;
}

int ff_get_line(AVIOContext *s, char *buf, int maxlen)
{
    int i = 0;
    char c;

    do {
        c = avio_r8(s);
        if (c && i < maxlen-1)
            buf[i++] = c;
    } while (c != '\n' && c);

    buf[i] = 0;
    return i;
}

#define GET_STR16(type, read) \
    int avio_get_str16 ##type(AVIOContext *pb, int maxlen, char *buf, int buflen)\
{\
    char* q = buf;\
    int ret = 0;\
    while (ret + 1 < maxlen) {\
        uint8_t tmp;\
        uint32_t ch;\
        GET_UTF16(ch, (ret += 2) <= maxlen ? read(pb) : 0, break;)\
        if (!ch)\
            break;\
        PUT_UTF8(ch, tmp, if (q - buf < buflen - 1) *q++ = tmp;)\
    }\
    *q = 0;\
    return ret;\
}\

GET_STR16(le, avio_rl16)
GET_STR16(be, avio_rb16)

#undef GET_STR16

uint64_t avio_rb64(AVIOContext *s)
{
    uint64_t val;
    val = (uint64_t)avio_rb32(s) << 32;
    val |= (uint64_t)avio_rb32(s);
    return val;
}

uint64_t ff_get_v(AVIOContext *bc){
    uint64_t val = 0;
    int tmp;

    do{
        tmp = avio_r8(bc);
        val= (val<<7) + (tmp&127);
    }while(tmp&128);
    return val;
}

int url_fdopen(AVIOContext **s, URLContext *h)
{
    uint8_t *buffer;
    int buffer_size, max_packet_size;

    max_packet_size = url_get_max_packet_size(h);
    if (max_packet_size) {
        buffer_size = max_packet_size; /* no need to bufferize more than one packet */
    } else {
        buffer_size = IO_BUFFER_SIZE;
    }
    buffer = av_malloc(buffer_size);
    if (!buffer)
        return AVERROR(ENOMEM);

    *s = av_mallocz(sizeof(AVIOContext));
    if(!*s) {
        av_free(buffer);
        return AVERROR(ENOMEM);
    }

    if (ffio_init_context(*s, buffer, buffer_size,
                      (h->flags & URL_WRONLY || h->flags & URL_RDWR), h,
                      url_read, url_write, url_seek) < 0) {
        av_free(buffer);
        av_freep(s);
        return AVERROR(EIO);
    }
    (*s)->is_streamed = h->is_streamed;
    (*s)->max_packet_size = max_packet_size;
    if(h->prot) {
        (*s)->read_pause = (int (*)(void *, int))h->prot->url_read_pause;
        (*s)->read_seek  = (int64_t (*)(void *, int, int64_t, int))h->prot->url_read_seek;
    }
    return 0;
}

int url_setbufsize(AVIOContext *s, int buf_size)
{
    uint8_t *buffer;
    buffer = av_malloc(buf_size);
    if (!buffer)
        return AVERROR(ENOMEM);

    av_free(s->buffer);
    s->buffer = buffer;
    s->buffer_size = buf_size;
    s->buf_ptr = buffer;
    url_resetbuf(s, s->write_flag ? URL_WRONLY : URL_RDONLY);
    return 0;
}

#if FF_API_URL_RESETBUF
int url_resetbuf(AVIOContext *s, int flags)
#else
static int url_resetbuf(AVIOContext *s, int flags)
#endif
{
#if FF_API_URL_RESETBUF
    if (flags & URL_RDWR)
        return AVERROR(EINVAL);
#else
    assert(flags == URL_WRONLY || flags == URL_RDONLY);
#endif

    if (flags & URL_WRONLY) {
        s->buf_end = s->buffer + s->buffer_size;
        s->write_flag = 1;
    } else {
        s->buf_end = s->buffer;
        s->write_flag = 0;
    }
    return 0;
}

int ff_rewind_with_probe_data(AVIOContext *s, unsigned char *buf, int buf_size)
{
    int64_t buffer_start;
    int buffer_size;
    int overlap, new_size, alloc_size;

    if (s->write_flag)
        return AVERROR(EINVAL);

    buffer_size = s->buf_end - s->buffer;

    /* the buffers must touch or overlap */
    if ((buffer_start = s->pos - buffer_size) > buf_size)
        return AVERROR(EINVAL);

    overlap = buf_size - buffer_start;
    new_size = buf_size + buffer_size - overlap;

    alloc_size = FFMAX(s->buffer_size, new_size);
    if (alloc_size > buf_size)
        if (!(buf = av_realloc(buf, alloc_size)))
            return AVERROR(ENOMEM);

    if (new_size > buf_size) {
        memcpy(buf + buf_size, s->buffer + overlap, buffer_size - overlap);
        buf_size = new_size;
    }

    av_free(s->buffer);
    s->buf_ptr = s->buffer = buf;
    s->buffer_size = alloc_size;
    s->pos = buf_size;
    s->buf_end = s->buf_ptr + buf_size;
    s->eof_reached = 0;
    s->must_flush = 0;

    return 0;
}

int url_fopen(AVIOContext **s, const char *filename, int flags)
{
    URLContext *h;
    int err;

    err = url_open(&h, filename, flags);
    if (err < 0)
        return err;
    err = url_fdopen(s, h);
    if (err < 0) {
        url_close(h);
        return err;
    }
    return 0;
}

int url_fclose(AVIOContext *s)
{
    URLContext *h = s->opaque;

    av_free(s->buffer);
    av_free(s);
    return url_close(h);
}

URLContext *url_fileno(AVIOContext *s)
{
    return s->opaque;
}

#if CONFIG_MUXERS
int url_fprintf(AVIOContext *s, const char *fmt, ...)
{
    va_list ap;
    char buf[4096];
    int ret;

    va_start(ap, fmt);
    ret = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    put_buffer(s, buf, strlen(buf));
    return ret;
}
#endif //CONFIG_MUXERS

char *url_fgets(AVIOContext *s, char *buf, int buf_size)
{
    int c;
    char *q;

    c = url_fgetc(s);
    if (c == EOF)
        return NULL;
    q = buf;
    for(;;) {
        if (c == EOF || c == '\n')
            break;
        if ((q - buf) < buf_size - 1)
            *q++ = c;
        c = url_fgetc(s);
    }
    if (buf_size > 0)
        *q = '\0';
    return buf;
}

int url_fget_max_packet_size(AVIOContext *s)
{
    return s->max_packet_size;
}

int av_url_read_fpause(AVIOContext *s, int pause)
{
    if (!s->read_pause)
        return AVERROR(ENOSYS);
    return s->read_pause(s->opaque, pause);
}

int64_t av_url_read_fseek(AVIOContext *s, int stream_index,
                          int64_t timestamp, int flags)
{
    URLContext *h = s->opaque;
    int64_t ret;
    if (!s->read_seek)
        return AVERROR(ENOSYS);
    ret = s->read_seek(h, stream_index, timestamp, flags);
    if(ret >= 0) {
        int64_t pos;
        s->buf_ptr = s->buf_end; // Flush buffer
        pos = s->seek(h, 0, SEEK_CUR);
        if (pos >= 0)
            s->pos = pos;
        else if (pos != AVERROR(ENOSYS))
            ret = pos;
    }
    return ret;
}

/* url_open_dyn_buf and url_close_dyn_buf are used in rtp.c to send a response
 * back to the server even if CONFIG_MUXERS is false. */
#if CONFIG_MUXERS || CONFIG_NETWORK
/* buffer handling */
int url_open_buf(AVIOContext **s, uint8_t *buf, int buf_size, int flags)
{
    int ret;
    *s = av_mallocz(sizeof(AVIOContext));
    if(!*s)
        return AVERROR(ENOMEM);
    ret = ffio_init_context(*s, buf, buf_size,
                        (flags & URL_WRONLY || flags & URL_RDWR),
                        NULL, NULL, NULL, NULL);
    if(ret != 0)
        av_freep(s);
    return ret;
}

int url_close_buf(AVIOContext *s)
{
    put_flush_packet(s);
    return s->buf_ptr - s->buffer;
}

/* output in a dynamic buffer */

typedef struct DynBuffer {
    int pos, size, allocated_size;
    uint8_t *buffer;
    int io_buffer_size;
    uint8_t io_buffer[1];
} DynBuffer;

static int dyn_buf_write(void *opaque, uint8_t *buf, int buf_size)
{
    DynBuffer *d = opaque;
    unsigned new_size, new_allocated_size;

    /* reallocate buffer if needed */
    new_size = d->pos + buf_size;
    new_allocated_size = d->allocated_size;
    if(new_size < d->pos || new_size > INT_MAX/2)
        return -1;
    while (new_size > new_allocated_size) {
        if (!new_allocated_size)
            new_allocated_size = new_size;
        else
            new_allocated_size += new_allocated_size / 2 + 1;
    }

    if (new_allocated_size > d->allocated_size) {
        d->buffer = av_realloc(d->buffer, new_allocated_size);
        if(d->buffer == NULL)
             return AVERROR(ENOMEM);
        d->allocated_size = new_allocated_size;
    }
    memcpy(d->buffer + d->pos, buf, buf_size);
    d->pos = new_size;
    if (d->pos > d->size)
        d->size = d->pos;
    return buf_size;
}

static int dyn_packet_buf_write(void *opaque, uint8_t *buf, int buf_size)
{
    unsigned char buf1[4];
    int ret;

    /* packetized write: output the header */
    AV_WB32(buf1, buf_size);
    ret= dyn_buf_write(opaque, buf1, 4);
    if(ret < 0)
        return ret;

    /* then the data */
    return dyn_buf_write(opaque, buf, buf_size);
}

static int64_t dyn_buf_seek(void *opaque, int64_t offset, int whence)
{
    DynBuffer *d = opaque;

    if (whence == SEEK_CUR)
        offset += d->pos;
    else if (whence == SEEK_END)
        offset += d->size;
    if (offset < 0 || offset > 0x7fffffffLL)
        return -1;
    d->pos = offset;
    return 0;
}

static int url_open_dyn_buf_internal(AVIOContext **s, int max_packet_size)
{
    DynBuffer *d;
    int ret;
    unsigned io_buffer_size = max_packet_size ? max_packet_size : 1024;

    if(sizeof(DynBuffer) + io_buffer_size < io_buffer_size)
        return -1;
    d = av_mallocz(sizeof(DynBuffer) + io_buffer_size);
    if (!d)
        return AVERROR(ENOMEM);
    *s = av_mallocz(sizeof(AVIOContext));
    if(!*s) {
        av_free(d);
        return AVERROR(ENOMEM);
    }
    d->io_buffer_size = io_buffer_size;
    ret = ffio_init_context(*s, d->io_buffer, io_buffer_size,
                        1, d, NULL,
                        max_packet_size ? dyn_packet_buf_write : dyn_buf_write,
                        max_packet_size ? NULL : dyn_buf_seek);
    if (ret == 0) {
        (*s)->max_packet_size = max_packet_size;
    } else {
        av_free(d);
        av_freep(s);
    }
    return ret;
}

int url_open_dyn_buf(AVIOContext **s)
{
    return url_open_dyn_buf_internal(s, 0);
}

int url_open_dyn_packet_buf(AVIOContext **s, int max_packet_size)
{
    if (max_packet_size <= 0)
        return -1;
    return url_open_dyn_buf_internal(s, max_packet_size);
}

int url_close_dyn_buf(AVIOContext *s, uint8_t **pbuffer)
{
    DynBuffer *d = s->opaque;
    int size;
    static const char padbuf[FF_INPUT_BUFFER_PADDING_SIZE] = {0};
    int padding = 0;

    /* don't attempt to pad fixed-size packet buffers */
    if (!s->max_packet_size) {
        put_buffer(s, padbuf, sizeof(padbuf));
        padding = FF_INPUT_BUFFER_PADDING_SIZE;
    }

    put_flush_packet(s);

    *pbuffer = d->buffer;
    size = d->size;
    av_free(d);
    av_free(s);
    return size - padding;
}
#endif /* CONFIG_MUXERS || CONFIG_NETWORK */
