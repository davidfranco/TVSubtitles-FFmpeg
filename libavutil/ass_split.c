/*
 * SSA/ASS spliting functions
 * Copyright (c) 2010  Aurelien Jacobs <aurel@gnuage.org>
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

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "libavutil/error.h"
#include "libavutil/macros.h"
#include "libavutil/mem.h"
#include "ass_split_internal.h"

typedef enum {
    ASS_STR,
    ASS_INT,
    ASS_FLT,
    ASS_COLOR,
    ASS_TIMESTAMP,
    ASS_ALGN,
} ASSFieldType;

typedef struct {
    const char *name;
    int type;
    int offset;
} ASSFields;

typedef struct {
    const char *section;
    const char *format_header;
    const char *fields_header;
    int         size;
    int         offset;
    int         offset_count;
    ASSFields   fields[24];
} ASSSection;

static const ASSSection ass_sections[] = {
    { .section       = "Script Info",
      .offset        = offsetof(ASS, script_info),
      .fields = {{"ScriptType", ASS_STR, offsetof(ASSScriptInfo, script_type)},
                 {"Collisions", ASS_STR, offsetof(ASSScriptInfo, collisions) },
                 {"PlayResX",   ASS_INT, offsetof(ASSScriptInfo, play_res_x) },
                 {"PlayResY",   ASS_INT, offsetof(ASSScriptInfo, play_res_y) },
                 {"Timer",      ASS_FLT, offsetof(ASSScriptInfo, timer)      },
                 {0},
        }
    },
    { .section       = "V4+ Styles",
      .format_header = "Format",
      .fields_header = "Style",
      .size          = sizeof(ASSStyle),
      .offset        = offsetof(ASS, styles),
      .offset_count  = offsetof(ASS, styles_count),
      .fields = {{"Name",            ASS_STR,   offsetof(ASSStyle, name)           },
                 {"Fontname",        ASS_STR,   offsetof(ASSStyle, font_name)      },
                 {"Fontsize",        ASS_INT,   offsetof(ASSStyle, font_size)      },
                 {"PrimaryColour",   ASS_COLOR, offsetof(ASSStyle, primary_color)  },
                 {"SecondaryColour", ASS_COLOR, offsetof(ASSStyle, secondary_color)},
                 {"OutlineColour",   ASS_COLOR, offsetof(ASSStyle, outline_color)  },
                 {"BackColour",      ASS_COLOR, offsetof(ASSStyle, back_color)     },
                 {"Bold",            ASS_INT,   offsetof(ASSStyle, bold)           },
                 {"Italic",          ASS_INT,   offsetof(ASSStyle, italic)         },
                 {"Underline",       ASS_INT,   offsetof(ASSStyle, underline)      },
                 {"StrikeOut",       ASS_INT,   offsetof(ASSStyle, strikeout)      },
                 {"ScaleX",          ASS_FLT,   offsetof(ASSStyle, scalex)         },
                 {"ScaleY",          ASS_FLT,   offsetof(ASSStyle, scaley)         },
                 {"Spacing",         ASS_FLT,   offsetof(ASSStyle, spacing)        },
                 {"Angle",           ASS_FLT,   offsetof(ASSStyle, angle)          },
                 {"BorderStyle",     ASS_INT,   offsetof(ASSStyle, border_style)   },
                 {"Outline",         ASS_FLT,   offsetof(ASSStyle, outline)        },
                 {"Shadow",          ASS_FLT,   offsetof(ASSStyle, shadow)         },
                 {"Alignment",       ASS_INT,   offsetof(ASSStyle, alignment)      },
                 {"MarginL",         ASS_INT,   offsetof(ASSStyle, margin_l)       },
                 {"MarginR",         ASS_INT,   offsetof(ASSStyle, margin_r)       },
                 {"MarginV",         ASS_INT,   offsetof(ASSStyle, margin_v)       },
                 {"Encoding",        ASS_INT,   offsetof(ASSStyle, encoding)       },
                 {0},
        }
    },
    { .section       = "V4 Styles",
      .format_header = "Format",
      .fields_header = "Style",
      .size          = sizeof(ASSStyle),
      .offset        = offsetof(ASS, styles),
      .offset_count  = offsetof(ASS, styles_count),
      .fields = {{"Name",            ASS_STR,   offsetof(ASSStyle, name)           },
                 {"Fontname",        ASS_STR,   offsetof(ASSStyle, font_name)      },
                 {"Fontsize",        ASS_INT,   offsetof(ASSStyle, font_size)      },
                 {"PrimaryColour",   ASS_COLOR, offsetof(ASSStyle, primary_color)  },
                 {"SecondaryColour", ASS_COLOR, offsetof(ASSStyle, secondary_color)},
                 {"TertiaryColour",  ASS_COLOR, offsetof(ASSStyle, outline_color)  },
                 {"BackColour",      ASS_COLOR, offsetof(ASSStyle, back_color)     },
                 {"Bold",            ASS_INT,   offsetof(ASSStyle, bold)           },
                 {"Italic",          ASS_INT,   offsetof(ASSStyle, italic)         },
                 {"BorderStyle",     ASS_INT,   offsetof(ASSStyle, border_style)   },
                 {"Outline",         ASS_FLT,   offsetof(ASSStyle, outline)        },
                 {"Shadow",          ASS_FLT,   offsetof(ASSStyle, shadow)         },
                 {"Alignment",       ASS_ALGN,  offsetof(ASSStyle, alignment)      },
                 {"MarginL",         ASS_INT,   offsetof(ASSStyle, margin_l)       },
                 {"MarginR",         ASS_INT,   offsetof(ASSStyle, margin_r)       },
                 {"MarginV",         ASS_INT,   offsetof(ASSStyle, margin_v)       },
                 {"AlphaLevel",      ASS_INT,   offsetof(ASSStyle, alpha_level)    },
                 {"Encoding",        ASS_INT,   offsetof(ASSStyle, encoding)       },
                 {0},
        }
    },
    { .section       = "Events",
      .format_header = "Format",
      .fields_header = "Dialogue",
      .size          = sizeof(ASSDialog),
      .offset        = offsetof(ASS, dialogs),
      .offset_count  = offsetof(ASS, dialogs_count),
      .fields = {{"Layer",   ASS_INT,        offsetof(ASSDialog, layer)   },
                 {"Start",   ASS_TIMESTAMP,  offsetof(ASSDialog, start)   },
                 {"End",     ASS_TIMESTAMP,  offsetof(ASSDialog, end)     },
                 {"Style",   ASS_STR,        offsetof(ASSDialog, style)   },
                 {"Name",    ASS_STR,        offsetof(ASSDialog, name)    },
                 {"MarginL", ASS_INT,        offsetof(ASSDialog, margin_l)},
                 {"MarginR", ASS_INT,        offsetof(ASSDialog, margin_r)},
                 {"MarginV", ASS_INT,        offsetof(ASSDialog, margin_v)},
                 {"Effect",  ASS_STR,        offsetof(ASSDialog, effect)  },
                 {"Text",    ASS_STR,        offsetof(ASSDialog, text)    },
                 {0},
        }
    },
};


typedef int (*ASSConvertFunc)(void *dest, const char *buf, int len);

static int convert_str(void *dest, const char *buf, int len)
{
    char *str = av_malloc(len + 1);
    if (str) {
        memcpy(str, buf, len);
        str[len] = 0;
        if (*(void **)dest)
            av_free(*(void **)dest);
        *(char **)dest = str;
    }
    return !str;
}
static int convert_int(void *dest, const char *buf, int len)
{
    return sscanf(buf, "%d", (int *)dest) == 1;
}
static int convert_flt(void *dest, const char *buf, int len)
{
    return sscanf(buf, "%f", (float *)dest) == 1;
}
static int convert_color(void *dest, const char *buf, int len)
{
    return sscanf(buf, "&H%8x", (int *)dest) == 1 ||
           sscanf(buf, "%d",    (int *)dest) == 1;
}
static int convert_timestamp(void *dest, const char *buf, int len)
{
    int c, h, m, s, cs;
    if ((c = sscanf(buf, "%d:%02d:%02d.%02d", &h, &m, &s, &cs)) == 4)
        *(int *)dest = 360000*h + 6000*m + 100*s + cs;
    return c == 4;
}
static int convert_alignment(void *dest, const char *buf, int len)
{
    int a;
    if (sscanf(buf, "%d", &a) == 1) {
        /* convert V4 Style alignment to V4+ Style */
        *(int *)dest = a + ((a&4) >> 1) - 5*!!(a&8);
        return 1;
    }
    return 0;
}

static const ASSConvertFunc convert_func[] = {
    [ASS_STR]       = convert_str,
    [ASS_INT]       = convert_int,
    [ASS_FLT]       = convert_flt,
    [ASS_COLOR]     = convert_color,
    [ASS_TIMESTAMP] = convert_timestamp,
    [ASS_ALGN]      = convert_alignment,
};


struct ASSSplitContext {
    ASS ass;
    int current_section;
    int field_number[FF_ARRAY_ELEMS(ass_sections)];
    int *field_order[FF_ARRAY_ELEMS(ass_sections)];
};


static uint8_t *realloc_section_array(ASSSplitContext *ctx)
{
    const ASSSection *section = &ass_sections[ctx->current_section];
    int *count = (int *)((uint8_t *)&ctx->ass + section->offset_count);
    void **section_ptr = (void **)((uint8_t *)&ctx->ass + section->offset);
    uint8_t *tmp = av_realloc_array(*section_ptr, (*count+1), section->size);
    if (!tmp)
        return NULL;
    *section_ptr = tmp;
    tmp += *count * section->size;
    memset(tmp, 0, section->size);
    (*count)++;
    return tmp;
}

static inline int is_eol(char buf)
{
    return buf == '\r' || buf == '\n' || buf == 0;
}

static inline const char *skip_space(const char *buf)
{
    while (*buf == ' ')
        buf++;
    return buf;
}

static int *get_default_field_orders(const ASSSection *section, int *number)
{
    int i;
    int *order = av_malloc_array(FF_ARRAY_ELEMS(section->fields), sizeof(*order));

    if (!order)
        return NULL;
    for (i = 0; section->fields[i].name; i++)
        order[i] = i;
    *number = i;
    while (i < FF_ARRAY_ELEMS(section->fields))
        order[i++] = -1;
    return order;
}

static const char *ass_split_section(ASSSplitContext *ctx, const char *buf)
{
    const ASSSection *section = &ass_sections[ctx->current_section];
    int *number = &ctx->field_number[ctx->current_section];
    int *order = ctx->field_order[ctx->current_section];
    int i, len;

    while (buf && *buf) {
        if (buf[0] == '[') {
            ctx->current_section = -1;
            break;
        }
        if (buf[0] == ';' || (buf[0] == '!' && buf[1] == ':'))
            goto next_line; // skip comments

        len = strcspn(buf, ":\r\n");
        if (buf[len] == ':' &&
            (!section->fields_header || strncmp(buf, section->fields_header, len))) {
            for (i = 0; i < FF_ARRAY_ELEMS(ass_sections); i++) {
                if (ass_sections[i].fields_header &&
                    !strncmp(buf, ass_sections[i].fields_header, len)) {
                    ctx->current_section = i;
                    section = &ass_sections[ctx->current_section];
                    number = &ctx->field_number[ctx->current_section];
                    order = ctx->field_order[ctx->current_section];
                    break;
                }
            }
        }
        if (section->format_header && !order) {
            len = strlen(section->format_header);
            if (!strncmp(buf, section->format_header, len) && buf[len] == ':') {
                buf += len + 1;
                while (!is_eol(*buf)) {
                    buf = skip_space(buf);
                    len = strcspn(buf, ", \r\n");
                    if (av_reallocp_array(&order, (*number + 1), sizeof(*order)) != 0)
                        return NULL;

                    order[*number] = -1;
                    for (i=0; section->fields[i].name; i++)
                        if (!strncmp(buf, section->fields[i].name, len)) {
                            order[*number] = i;
                            break;
                        }
                    (*number)++;
                    buf = skip_space(buf + len + (buf[len] == ','));
                }
                ctx->field_order[ctx->current_section] = order;
                goto next_line;
            }
        }
        if (section->fields_header) {
            len = strlen(section->fields_header);
            if (!strncmp(buf, section->fields_header, len) && buf[len] == ':') {
                uint8_t *ptr, *struct_ptr = realloc_section_array(ctx);
                if (!struct_ptr)  return NULL;

                /* No format header line found so far, assume default */
                if (!order) {
                    order = get_default_field_orders(section, number);
                    if (!order)
                        return NULL;
                    ctx->field_order[ctx->current_section] = order;
                }

                buf += len + 1;
                for (i=0; !is_eol(*buf) && i < *number; i++) {
                    int last = i == *number - 1;
                    buf = skip_space(buf);
                    len = strcspn(buf, last ? "\r\n" : ",\r\n");
                    if (order[i] >= 0) {
                        ASSFieldType type = section->fields[order[i]].type;
                        ptr = struct_ptr + section->fields[order[i]].offset;
                        convert_func[type](ptr, buf, len);
                    }
                    buf += len;
                    if (!last && *buf) buf++;
                    buf = skip_space(buf);
                }
            }
        } else {
            len = strcspn(buf, ":\r\n");
            if (buf[len] == ':') {
                for (i=0; section->fields[i].name; i++)
                    if (!strncmp(buf, section->fields[i].name, len)) {
                        ASSFieldType type = section->fields[i].type;
                        uint8_t *ptr = (uint8_t *)&ctx->ass + section->offset;
                        ptr += section->fields[i].offset;
                        buf = skip_space(buf + len + 1);
                        convert_func[type](ptr, buf, strcspn(buf, "\r\n"));
                        break;
                    }
            }
        }
next_line:
        buf += strcspn(buf, "\n");
        buf += !!*buf;
    }
    return buf;
}

static int ass_split(ASSSplitContext *ctx, const char *buf)
{
    char c, section[16];
    int i;

    if (ctx->current_section >= 0)
        buf = ass_split_section(ctx, buf);

    while (buf && *buf) {
        if (sscanf(buf, "[%15[0-9A-Za-z+ ]]%c", section, &c) == 2) {
            buf += strcspn(buf, "\n");
            buf += !!*buf;
            for (i=0; i<FF_ARRAY_ELEMS(ass_sections); i++)
                if (!strcmp(section, ass_sections[i].section)) {
                    ctx->current_section = i;
                    buf = ass_split_section(ctx, buf);
                }
        } else {
            buf += strcspn(buf, "\n");
            buf += !!*buf;
        }
    }
    return buf ? 0 : AVERROR_INVALIDDATA;
}

ASSSplitContext *avpriv_ass_split(const char *buf)
{
    ASSSplitContext *ctx = av_mallocz(sizeof(*ctx));
    if (!ctx)
        return NULL;
    if (buf && !strncmp(buf, "\xef\xbb\xbf", 3)) // Skip UTF-8 BOM header
        buf += 3;
    ctx->current_section = -1;
    if (ass_split(ctx, buf) < 0) {
        avpriv_ass_split_free(ctx);
        return NULL;
    }
    return ctx;
}

static void free_section(ASSSplitContext *ctx, const ASSSection *section)
{
    uint8_t *ptr = (uint8_t *)&ctx->ass + section->offset;
    int i, j, *count, c = 1;

    if (section->format_header) {
        ptr   = *(void **)ptr;
        count = (int *)((uint8_t *)&ctx->ass + section->offset_count);
    } else
        count = &c;

    if (ptr)
        for (i=0; i<*count; i++, ptr += section->size)
            for (j=0; section->fields[j].name; j++) {
                const ASSFields *field = &section->fields[j];
                if (field->type == ASS_STR)
                    av_freep(ptr + field->offset);
            }
    *count = 0;

    if (section->format_header)
        av_freep((uint8_t *)&ctx->ass + section->offset);
}

void avpriv_ass_free_dialog(ASSDialog **dialogp)
{
    ASSDialog *dialog = *dialogp;
    if (!dialog)
        return;
    av_freep(&dialog->style);
    av_freep(&dialog->name);
    av_freep(&dialog->effect);
    av_freep(&dialog->text);
    av_freep(dialogp);
}

ASSDialog *avpriv_ass_split_dialog(ASSSplitContext *ctx, const char *buf)
{
    int i;
    static const ASSFields fields[] = {
        {"ReadOrder", ASS_INT, offsetof(ASSDialog, readorder)},
        {"Layer",     ASS_INT, offsetof(ASSDialog, layer)    },
        {"Style",     ASS_STR, offsetof(ASSDialog, style)    },
        {"Name",      ASS_STR, offsetof(ASSDialog, name)     },
        {"MarginL",   ASS_INT, offsetof(ASSDialog, margin_l) },
        {"MarginR",   ASS_INT, offsetof(ASSDialog, margin_r) },
        {"MarginV",   ASS_INT, offsetof(ASSDialog, margin_v) },
        {"Effect",    ASS_STR, offsetof(ASSDialog, effect)   },
        {"Text",      ASS_STR, offsetof(ASSDialog, text)     },
    };

    ASSDialog *dialog = av_mallocz(sizeof(*dialog));
    if (!dialog)
        return NULL;

    for (i = 0; i < FF_ARRAY_ELEMS(fields); i++) {
        size_t len;
        const int last = i == FF_ARRAY_ELEMS(fields) - 1;
        const ASSFieldType type = fields[i].type;
        uint8_t *ptr = (uint8_t *)dialog + fields[i].offset;
        buf = skip_space(buf);
        len = last ? strlen(buf) : strcspn(buf, ",");
        if (len >= INT_MAX) {
            avpriv_ass_free_dialog(&dialog);
            return NULL;
        }
        convert_func[type](ptr, buf, len);
        buf += len;
        if (*buf) buf++;
    }
    return dialog;
}

void avpriv_ass_split_free(ASSSplitContext *ctx)
{
    if (ctx) {
        int i;
        for (i=0; i<FF_ARRAY_ELEMS(ass_sections); i++) {
            free_section(ctx, &ass_sections[i]);
            av_freep(&(ctx->field_order[i]));
        }
        av_free(ctx);
    }
}

static int ass_remove_empty_braces(AVBPrint* buffer)
{
    char* tmp;
    int ret = 0, n = 0;

    if (buffer == NULL || buffer->len == 0 || !av_bprint_is_complete(buffer))
        return 0;

    ret = av_bprint_finalize(buffer, &tmp);
    if (ret)
        return ret;

    for (unsigned i = 0; i < buffer->len; i++) {
        if (tmp[i] == '{' && tmp[i+1] == '}')
            i++;
        else
            tmp[n++] = tmp[i];
    }

    tmp[n++] = '\0';

    av_bprint_init(buffer, n, n);
    av_bprint_append_data(buffer, tmp, n - 1);
    av_free(tmp);

    return ret;
}

static void ass_write_filtered_line(AVBPrint* buffer, const char *buf, int len, enum ASSSplitComponents keep_flags, enum ASSSplitComponents split_component)
{
    if (buffer == NULL || buf == NULL || len == 0)
        return;

    if (split_component != ASS_SPLIT_ANY && !(keep_flags & split_component))
        return;


    av_bprint_append_data(buffer, buf, len - 1);
}

int avpriv_ass_filter_override_codes(const ASSCodesCallbacks *callbacks, void *priv, const char *buf, AVBPrint* outbuffer, enum ASSSplitComponents keep_flags)
{
    const char *text = NULL;
    char new_line[2];
    int text_len = 0, ret = 0;

    while (buf && *buf) {

        if (text && (sscanf(buf, "\\%1[nN]", new_line) == 1 || !strncmp(buf, "{\\", 2))) {
            ass_write_filtered_line(outbuffer, text, text_len + 1, keep_flags, ASS_SPLIT_TEXT | ASS_SPLIT_TEXT2);

            if (callbacks->text)
                callbacks->text(priv, text, text_len);
            text = NULL;
        }

        if (sscanf(buf, "\\%1[nN]", new_line) == 1) {
            if (callbacks->new_line)
                callbacks->new_line(priv, new_line[0] == 'N');
            ass_write_filtered_line(outbuffer, buf, 3, keep_flags, ASS_SPLIT_ANY);
            buf += 2;
        } else if (!strncmp(buf, "{\\", 2)) {
            ass_write_filtered_line(outbuffer, buf, 2, keep_flags, ASS_SPLIT_ANY);
            buf++;
            while (*buf == '\\') {
                char style[4], c[2], axis[3], sep[3], c_num[2] = "0", tmp[128] = {0};
                unsigned int color = 0xFFFFFFFF;
                int len, size = -1, an = -1, alpha = -1, scale = 0;
                float f1 = 1;
                int x1, y1, x2, y2, x3, t1 = -1, t2 = -1, t3 = -1, t4 = -1, accel = 1;
                if (sscanf(buf, "\\%1[bisu]%1[01\\}]%n", style, c, &len) > 1) {
                    int close = c[0] == '0' ? 1 : c[0] == '1' ? 0 : -1;
                    len += close != -1;
                    switch (c[0]) {
                    case 'b':
                        ass_write_filtered_line(outbuffer, buf, len, keep_flags, ASS_SPLIT_FONT_BOLD);
                        break;
                    case 'u':
                        ass_write_filtered_line(outbuffer, buf, len, keep_flags, ASS_SPLIT_FONT_UNDERLINE);
                        break;
                    case 'i':
                        ass_write_filtered_line(outbuffer, buf, len, keep_flags, ASS_SPLIT_FONT_ITALIC);
                        break;
                    case 'a':
                        ass_write_filtered_line(outbuffer, buf, len, keep_flags, ASS_SPLIT_FONT_STRIKEOUT);
                        break;
                    }
                    if (callbacks->style)
                        callbacks->style(priv, style[0], close);
                } else if (sscanf(buf, "\\c%1[\\}]%n", sep, &len) > 0 ||
                           sscanf(buf, "\\c&H%X&%1[\\}]%n", &color, sep, &len) > 1 ||
                           sscanf(buf, "\\%1[1234]c%1[\\}]%n", c_num, sep, &len) > 1 ||
                           sscanf(buf, "\\%1[1234]c&H%X&%1[\\}]%n", c_num, &color, sep, &len) > 2) {
                    ass_write_filtered_line(outbuffer, buf, len, keep_flags, ASS_SPLIT_COLOR);
                    if (callbacks->color)
                        callbacks->color(priv, color, c_num[0] - '0');
                } else if (sscanf(buf, "\\alpha%1[\\}]%n", sep, &len) > 0 ||
                           sscanf(buf, "\\alpha&H%2X&%1[\\}]%n", &alpha, sep, &len) > 1 ||
                           sscanf(buf, "\\%1[1234]a%1[\\}]%n", c_num, sep, &len) > 1 ||
                           sscanf(buf, "\\%1[1234]a&H%2X&%1[\\}]%n", c_num, &alpha, sep, &len) > 2) {
                    ass_write_filtered_line(outbuffer, buf, len, keep_flags, ASS_SPLIT_ALPHA);
                    if (callbacks->alpha)
                        callbacks->alpha(priv, alpha, c_num[0] - '0');
                } else if (sscanf(buf, "\\fn%1[\\}]%n", sep, &len) > 0 ||
                           sscanf(buf, "\\fn%127[^\\}]%1[\\}]%n", tmp, sep, &len) > 1) {
                    ass_write_filtered_line(outbuffer, buf, len, keep_flags, ASS_SPLIT_FONT_NAME);
                    if (callbacks->font_name)
                        callbacks->font_name(priv, tmp[0] ? tmp : NULL);
                } else if (sscanf(buf, "\\fs%1[\\}]%n", sep, &len) > 0 ||
                           sscanf(buf, "\\fs%u%1[\\}]%n", &size, sep, &len) > 1) {
                    ass_write_filtered_line(outbuffer, buf, len, keep_flags, ASS_SPLIT_FONT_SIZE);
                    if (callbacks->font_size)
                        callbacks->font_size(priv, size);
                } else if (sscanf(buf, "\\fscx%1[\\}]%n", sep, &len) > 0 ||
                           sscanf(buf, "\\fscx%f%1[\\}]%n", &f1, sep, &len) > 1) {
                    ass_write_filtered_line(outbuffer, buf, len, keep_flags, ASS_SPLIT_FONT_SCALE);
                } else if (sscanf(buf, "\\fscy%1[\\}]%n", sep, &len) > 0 ||
                           sscanf(buf, "\\fscy%f%1[\\}]%n", &f1, sep, &len) > 1) {
                    ass_write_filtered_line(outbuffer, buf, len, keep_flags, ASS_SPLIT_FONT_SCALE);
                } else if (sscanf(buf, "\\fsp%1[\\}]%n", sep, &len) > 0 ||
                           sscanf(buf, "\\fsp%u%1[\\}]%n", &size, sep, &len) > 1) {
                    ass_write_filtered_line(outbuffer, buf, len, keep_flags, ASS_SPLIT_FONT_SPACING);
                } else if (sscanf(buf, "\\fe%1[\\}]%n", sep, &len) > 0 ||
                           sscanf(buf, "\\fe%u%1[\\}]%n", &size, sep, &len) > 1) {
                    ass_write_filtered_line(outbuffer, buf, len, keep_flags, ASS_SPLIT_FONT_CHARSET);
                } else if (sscanf(buf, "\\bord%1[\\}]%n", sep, &len) > 0 ||
                           sscanf(buf, "\\bord%u%1[\\}]%n", &size, sep, &len) > 1) {
                    ass_write_filtered_line(outbuffer, buf, len, keep_flags, ASS_SPLIT_TEXT_BORDER);
                } else if (sscanf(buf, "\\shad%1[\\}]%n", sep, &len) > 0 ||
                           sscanf(buf, "\\shad%u%1[\\}]%n", &size, sep, &len) > 1) {
                    ass_write_filtered_line(outbuffer, buf, len, keep_flags, ASS_SPLIT_TEXT_SHADOW);
                } else if (sscanf(buf, "\\fr%1[\\}]%n", sep, &len) > 0 ||
                           sscanf(buf, "\\fr%u%1[\\}]%n", &x1, sep, &len) > 1 ||
                           sscanf(buf, "\\fr%1[xyz]%1[\\}]%n", axis, sep, &len) > 1 ||
                           sscanf(buf, "\\fr%1[xyz]%u%1[\\}]%n", axis, &size, sep, &len) > 2) {
                    ass_write_filtered_line(outbuffer, buf, len, keep_flags, ASS_SPLIT_TEXT_ROTATE);
                } else if (sscanf(buf, "\\blur%1[\\}]%n", sep, &len) > 0 ||
                           sscanf(buf, "\\blur%u%1[\\}]%n", &size, sep, &len) > 1) {
                    ass_write_filtered_line(outbuffer, buf, len, keep_flags, ASS_SPLIT_TEXT_BLUR);
                } else if (sscanf(buf, "\\be%1[\\}]%n", sep, &len) > 0 ||
                           sscanf(buf, "\\be%u%1[\\}]%n", &size, sep, &len) > 1) {
                    ass_write_filtered_line(outbuffer, buf, len, keep_flags, ASS_SPLIT_TEXT_BLUR);
                } else if (sscanf(buf, "\\q%1[\\}]%n", sep, &len) > 0 ||
                           sscanf(buf, "\\q%u%1[\\}]%n", &size, sep, &len) > 1) {
                    ass_write_filtered_line(outbuffer, buf, len, keep_flags, ASS_SPLIT_TEXT_WRAP);
                } else if (sscanf(buf, "\\a%1[\\}]%n", sep, &len) > 0 ||
                           sscanf(buf, "\\a%2u%1[\\}]%n", &an, sep, &len) > 1 ||
                           sscanf(buf, "\\an%1[\\}]%n", sep, &len) > 0 ||
                           sscanf(buf, "\\an%1u%1[\\}]%n", &an, sep, &len) > 1) {
                    if (an != -1 && buf[2] != 'n')
                        an = (an&3) + (an&4 ? 6 : an&8 ? 3 : 0);
                    ass_write_filtered_line(outbuffer, buf, len, keep_flags, ASS_SPLIT_TEXT_ALIGNMENT);
                    if (callbacks->alignment)
                        callbacks->alignment(priv, an);
                } else if (sscanf(buf, "\\r%1[\\}]%n", sep, &len) > 0 ||
                           sscanf(buf, "\\r%127[^\\}]%1[\\}]%n", tmp, sep, &len) > 1) {
                    ass_write_filtered_line(outbuffer, buf, len, keep_flags, ASS_SPLIT_CANCELLING);
                    if (callbacks->cancel_overrides)
                        callbacks->cancel_overrides(priv, tmp);
                } else if (sscanf(buf, "\\move(%d,%d,%d,%d)%1[\\}]%n", &x1, &y1, &x2, &y2, sep, &len) > 4 ||
                           sscanf(buf, "\\move(%d,%d,%d,%d,%d,%d)%1[\\}]%n", &x1, &y1, &x2, &y2, &t1, &t2, sep, &len) > 6) {
                    ass_write_filtered_line(outbuffer, buf, len, keep_flags, ASS_SPLIT_MOVE);
                    if (callbacks->move)
                        callbacks->move(priv, x1, y1, x2, y2, t1, t2);
                } else if (sscanf(buf, "\\pos(%d,%d)%1[\\}]%n", &x1, &y1, sep, &len) > 2) {
                    ass_write_filtered_line(outbuffer, buf, len, keep_flags, ASS_SPLIT_POS);
                    if (callbacks->move)
                        callbacks->move(priv, x1, y1, x1, y1, -1, -1);
                } else if (sscanf(buf, "\\org(%d,%d)%1[\\}]%n", &x1, &y1, sep, &len) > 2) {
                    ass_write_filtered_line(outbuffer, buf, len, keep_flags, ASS_SPLIT_ORIGIN);
                    if (callbacks->origin)
                        callbacks->origin(priv, x1, y1);
                } else if (sscanf(buf, "\\t(%1[\\}]%n", sep, &len) > 0 ||
                           sscanf(buf, "\\t(%d,%d,%1[\\}]%n", &t1, &t2, sep, &len) > 2 ||
                           sscanf(buf, "\\t(%d,%d,%d,%1[\\}]%n", &t1, &t2, &accel, sep, &len) > 3) {

                    len = strcspn(buf, ")") + 2;

                    ass_write_filtered_line(outbuffer, buf, len, keep_flags, ASS_SPLIT_ANIMATE);
                    if (callbacks->animate)
                        callbacks->animate(priv, t1, t2, accel, tmp);
                } else if (sscanf(buf, "\\fade(%d,%d,%d,%d,%d,%d,%d)%1[\\}]%n", &x1, &x2, &x3, &t1, &t2, &t3, &t4, sep, &len) > 7) {
                    ass_write_filtered_line(outbuffer, buf, len, keep_flags, ASS_SPLIT_FADE);
                } else if (sscanf(buf, "\\fad(%d,%d)%1[\\}]%n", &t1, &t2, sep, &len) > 2) {
                    ass_write_filtered_line(outbuffer, buf, len, keep_flags, ASS_SPLIT_FADE);
                } else if (sscanf(buf, "\\clip(%d,%d,%d,%d)%1[\\}]%n", &x1, &y1, &x2, &y2, sep, &len) > 4) {
                    ass_write_filtered_line(outbuffer, buf, len, keep_flags, ASS_SPLIT_CLIP);
                } else if (sscanf(buf, "\\p%1[\\}]%n", sep, &len) > 0 ||
                           sscanf(buf, "\\p%u%1[\\}]%n", &scale, sep, &len) > 1) {
                    ass_write_filtered_line(outbuffer, buf, len, keep_flags, ASS_SPLIT_DRAW);
                    if (callbacks->drawing_mode)
                        callbacks->drawing_mode(priv, scale);
                } else {
                    len = strcspn(buf+1, "\\}") + 2;  /* unknown code */
                    ass_write_filtered_line(outbuffer, buf, len, keep_flags, ASS_SPLIT_UNKNOWN);
             }
                buf += len - 1;
            }
            if (*buf++ != '}')
                return AVERROR_INVALIDDATA;

            ass_write_filtered_line(outbuffer, "}", 2, keep_flags, ASS_SPLIT_ANY);
     } else {
            if (!text) {
                text = buf;
                text_len = 1;
            } else
                text_len++;
            buf++;
        }
    }
    if (text)
        ass_write_filtered_line(outbuffer, text, text_len + 1, keep_flags, ASS_SPLIT_TEXT | ASS_SPLIT_TEXT2);
    if (text && callbacks->text)
        callbacks->text(priv, text, text_len);
    if (callbacks->end)
        callbacks->end(priv);

    if (outbuffer)
        ret = ass_remove_empty_braces(outbuffer);

    return ret;
}


int avpriv_ass_split_override_codes(const ASSCodesCallbacks *callbacks, void *priv,
                                const char *buf)
{
    return avpriv_ass_filter_override_codes(callbacks, priv, buf, NULL, 0);
}

ASSStyle *avpriv_ass_style_get(ASSSplitContext *ctx, const char *style)
{
    ASS *ass = &ctx->ass;
    int i;

    if (!style || !*style)
        style = "Default";
    for (i=0; i<ass->styles_count; i++)
        if (ass->styles[i].name && !strcmp(ass->styles[i].name, style))
            return ass->styles + i;
    return NULL;
}
