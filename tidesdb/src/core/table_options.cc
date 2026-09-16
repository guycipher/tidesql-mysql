/* Copyright (c) 2026 TidesDB Corp.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "table_options.h"

#include <cctype>
#include <cstring>
#include <limits>

namespace tidesdb
{
namespace table_options
{

namespace
{

/* one option's name, where it lives in the struct, and what it will accept. */
enum class kind
{
    boolean,
    number,
    compression, /* an enumerated value, numbered by the compression name list  */
    isolation,   /* an enumerated value, numbered by the isolation name list    */
};

struct spec
{
    const char *name;
    kind what;
    size_t offset;
    unsigned long long low;
    unsigned long long high;
};

#define TDB_OPT_OFFSET(member) offsetof(ha_table_option_struct, member)

/* The bounds match the session variables the same options default from, so a value set per table
   and the same value set for the session are accepted or refused alike. */
const spec specs[] = {
    {"keep_values_inline", kind::boolean, TDB_OPT_OFFSET(keep_values_inline), 0, 1},
    {"bloom_filter", kind::boolean, TDB_OPT_OFFSET(bloom_filter), 0, 1},
    {"encrypted", kind::boolean, TDB_OPT_OFFSET(encrypted), 0, 1},
    {"btree_klog_block_size", kind::number, TDB_OPT_OFFSET(btree_klog_block_size), 512,
     std::numeric_limits<unsigned long long>::max()},
    {"level_size_ratio", kind::number, TDB_OPT_OFFSET(level_size_ratio), 2, 100},
    {"min_levels", kind::number, TDB_OPT_OFFSET(min_levels), 1, 64},
    {"dividing_level_offset", kind::number, TDB_OPT_OFFSET(dividing_level_offset), 0, 64},
    {"bloom_fpr", kind::number, TDB_OPT_OFFSET(bloom_fpr), 1, 10000},
    {"l1_file_count_trigger", kind::number, TDB_OPT_OFFSET(l1_file_count_trigger), 1, 1024},
    {"tombstone_density_trigger", kind::number, TDB_OPT_OFFSET(tombstone_density_trigger), 0,
     10000},
    {"tombstone_density_min_entries", kind::number,
     TDB_OPT_OFFSET(tombstone_density_min_entries), 0,
     std::numeric_limits<unsigned long long>::max()},
    {"ttl", kind::number, TDB_OPT_OFFSET(ttl), 0, std::numeric_limits<unsigned long long>::max()},
    {"encryption_key_id", kind::number, TDB_OPT_OFFSET(encryption_key_id), 1, 255},
    {"compression", kind::compression, TDB_OPT_OFFSET(compression), 0, 0},
    {"isolation_level", kind::isolation, TDB_OPT_OFFSET(isolation_level), 0, 0},
};

#undef TDB_OPT_OFFSET

bool eq_nocase(const std::string &a, const char *b)
{
    size_t i = 0;
    for (; i < a.size() && b[i]; i++)
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
    return i == a.size() && b[i] == '\0';
}

void skip_space(const char *&p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
}

/* Read one JSON string.  Only the escapes a name or an enumerated value can plausibly carry are
   honoured; a table option holding a surrogate pair is not a case worth guessing at, so \u is
   refused rather than half-decoded. */
bool read_string(const char *&p, const char *end, std::string &out)
{
    if (p >= end || *p != '"') return false;
    p++;
    out.clear();
    while (p < end)
    {
        const char c = *p++;
        if (c == '"') return true;
        if (c != '\\')
        {
            out.push_back(c);
            continue;
        }
        if (p >= end) return false;
        const char esc = *p++;
        switch (esc)
        {
            case '"':
            case '\\':
            case '/':
                out.push_back(esc);
                break;
            case 'b':
                out.push_back('\b');
                break;
            case 'f':
                out.push_back('\f');
                break;
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            default:
                return false;
        }
    }
    return false;
}

/* Read one option value as text: a JSON string, a bare number, or true/false.  Whatever form it
   arrives in, it is handed to the per-option reader below as characters, so ENCRYPTED=1,
   ENCRYPTED=true and ENCRYPTED="YES" all mean the same thing. */
bool read_value(const char *&p, const char *end, std::string &out)
{
    skip_space(p, end);
    if (p >= end) return false;

    if (*p == '"') return read_string(p, end, out);

    const char *start = p;
    while (p < end && *p != ',' && *p != '}' && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
    {
        /* Anything that opens a nested value is not an option this engine has. */
        if (*p == '{' || *p == '[') return false;
        p++;
    }
    if (p == start) return false;
    out.assign(start, (size_t)(p - start));
    return true;
}

bool to_bool(const std::string &v, bool *out)
{
    if (eq_nocase(v, "true") || eq_nocase(v, "yes") || eq_nocase(v, "on") || v == "1")
    {
        *out = true;
        return true;
    }
    if (eq_nocase(v, "false") || eq_nocase(v, "no") || eq_nocase(v, "off") || v == "0")
    {
        *out = false;
        return true;
    }
    return false;
}

bool to_number(const std::string &v, unsigned long long *out)
{
    if (v.empty()) return false;

    unsigned long long n = 0;
    for (char c : v)
    {
        if (c < '0' || c > '9') return false;
        const unsigned long long digit = (unsigned long long)(c - '0');
        /* A value that would wrap is a different number from the one written, so it is refused
           rather than folded into range. */
        if (n > (std::numeric_limits<unsigned long long>::max() - digit) / 10) return false;
        n = n * 10 + digit;
    }
    *out = n;
    return true;
}

bool to_enum(const std::string &v, const char *const *names, unsigned int *out)
{
    if (!names) return false;
    for (unsigned int i = 0; names[i]; i++)
        if (eq_nocase(v, names[i]))
        {
            *out = i;
            return true;
        }
    /* An enumerated option also accepts the position itself, which is how the session variables
       report their own values. */
    unsigned long long n = 0;
    if (!to_number(v, &n)) return false;
    unsigned int count = 0;
    while (names[count]) count++;
    if (n >= count) return false;
    *out = (unsigned int)n;
    return true;
}

std::string quoted(const std::string &s)
{
    return "'" + s + "'";
}

}  // namespace

bool parse_attributes(const char *json, size_t len, const char *const *compression_names,
                      const char *const *isolation_names, ha_table_option_struct *inout,
                      std::string *error)
{
    if (!inout || !error) return false;
    if (!json || len == 0) return true; /* nothing said, defaults stand */

    const char *p = json;
    const char *const end = json + len;

    skip_space(p, end);
    if (p >= end)
        return true; /* whitespace only, same as nothing said */

    if (*p != '{')
    {
        *error = "the attribute must be a JSON object";
        return false;
    }
    p++;

    skip_space(p, end);
    if (p < end && *p == '}') return true; /* an empty object names no options */

    for (;;)
    {
        skip_space(p, end);

        std::string name;
        if (!read_string(p, end, name))
        {
            *error = "expected an option name";
            return false;
        }

        skip_space(p, end);
        if (p >= end || *p != ':')
        {
            *error = "expected ':' after " + quoted(name);
            return false;
        }
        p++;

        std::string value;
        if (!read_value(p, end, value))
        {
            *error = "expected a value for " + quoted(name);
            return false;
        }

        const spec *found = nullptr;
        for (const spec &s : specs)
            if (eq_nocase(name, s.name))
            {
                found = &s;
                break;
            }
        if (!found)
        {
            *error = "unknown option " + quoted(name);
            return false;
        }

        char *const base = reinterpret_cast<char *>(inout);
        switch (found->what)
        {
            case kind::boolean:
            {
                bool b = false;
                if (!to_bool(value, &b))
                {
                    *error = quoted(value) + " is not a yes-or-no value for " + quoted(name);
                    return false;
                }
                *reinterpret_cast<bool *>(base + found->offset) = b;
                break;
            }
            case kind::number:
            {
                unsigned long long n = 0;
                if (!to_number(value, &n))
                {
                    *error = quoted(value) + " is not a number for " + quoted(name);
                    return false;
                }
                if (n < found->low || n > found->high)
                {
                    *error = quoted(value) + " is out of range for " + quoted(name);
                    return false;
                }
                *reinterpret_cast<unsigned long long *>(base + found->offset) = n;
                break;
            }
            case kind::compression:
            case kind::isolation:
            {
                unsigned int e = 0;
                const char *const *names =
                    (found->what == kind::compression) ? compression_names : isolation_names;
                if (!to_enum(value, names, &e))
                {
                    *error = quoted(value) + " is not a recognised value for " + quoted(name);
                    return false;
                }
                *reinterpret_cast<unsigned int *>(base + found->offset) = e;
                break;
            }
        }

        skip_space(p, end);
        if (p < end && *p == ',')
        {
            p++;
            continue;
        }
        if (p < end && *p == '}')
        {
            p++;
            skip_space(p, end);
            if (p != end)
            {
                *error = "trailing text after the object";
                return false;
            }
            return true;
        }
        *error = "expected ',' or '}' after " + quoted(name);
        return false;
    }
}

namespace
{

const char *enum_name(const char *const *names, unsigned int value)
{
    if (!names) return nullptr;
    for (unsigned int i = 0; names[i]; i++)
        if (i == value) return names[i];
    return nullptr;
}

void append_number(std::string &out, const char *name, unsigned long long v, bool first)
{
    if (!first) out += ", ";
    out += '"';
    out += name;
    out += "\": ";
    out += std::to_string(v);
}

void append_bool(std::string &out, const char *name, bool v, bool first)
{
    if (!first) out += ", ";
    out += '"';
    out += name;
    out += "\": ";
    out += v ? "true" : "false";
}

/* An enumerated value whose name is missing is written as its position, which parse_attributes
   also accepts, so a value this build cannot name still round-trips rather than being lost. */
void append_enum(std::string &out, const char *name, unsigned int v, const char *const *names,
                 bool first)
{
    if (!first) out += ", ";
    out += '"';
    out += name;
    out += "\": ";
    if (const char *text = enum_name(names, v))
    {
        out += '"';
        out += text;
        out += '"';
    }
    else
        out += std::to_string(v);
}

}  // namespace

std::string serialize_attributes(const ha_table_option_struct &opts,
                                 const char *const *compression_names,
                                 const char *const *isolation_names)
{
    std::string out = "{";
    append_bool(out, "keep_values_inline", opts.keep_values_inline, true);
    append_bool(out, "bloom_filter", opts.bloom_filter, false);
    append_bool(out, "encrypted", opts.encrypted, false);
    append_number(out, "btree_klog_block_size", opts.btree_klog_block_size, false);
    append_number(out, "level_size_ratio", opts.level_size_ratio, false);
    append_number(out, "min_levels", opts.min_levels, false);
    append_number(out, "dividing_level_offset", opts.dividing_level_offset, false);
    append_number(out, "bloom_fpr", opts.bloom_fpr, false);
    append_number(out, "l1_file_count_trigger", opts.l1_file_count_trigger, false);
    append_number(out, "tombstone_density_trigger", opts.tombstone_density_trigger, false);
    append_number(out, "tombstone_density_min_entries", opts.tombstone_density_min_entries, false);
    append_number(out, "ttl", opts.ttl, false);
    append_number(out, "encryption_key_id", opts.encryption_key_id, false);
    append_enum(out, "compression", opts.compression, compression_names, false);
    append_enum(out, "isolation_level", opts.isolation_level, isolation_names, false);
    out += '}';
    return out;
}

bool parse_column_attributes(const char *json, size_t len, bool *is_ttl_source, std::string *error)
{
    if (!is_ttl_source || !error) return false;
    *is_ttl_source = false;
    if (!json || len == 0) return true;

    const char *p = json;
    const char *const end = json + len;

    skip_space(p, end);
    if (p >= end) return true;

    if (*p != '{')
    {
        *error = "the attribute must be a JSON object";
        return false;
    }
    p++;

    skip_space(p, end);
    if (p < end && *p == '}') return true;

    for (;;)
    {
        skip_space(p, end);

        std::string name;
        if (!read_string(p, end, name))
        {
            *error = "expected an option name";
            return false;
        }

        skip_space(p, end);
        if (p >= end || *p != ':')
        {
            *error = "expected ':' after " + quoted(name);
            return false;
        }
        p++;

        std::string value;
        if (!read_value(p, end, value))
        {
            *error = "expected a value for " + quoted(name);
            return false;
        }

        if (!eq_nocase(name, "ttl"))
        {
            *error = "unknown column option " + quoted(name);
            return false;
        }
        if (!to_bool(value, is_ttl_source))
        {
            *error = quoted(value) + " is not a yes-or-no value for " + quoted(name);
            return false;
        }

        skip_space(p, end);
        if (p < end && *p == ',')
        {
            p++;
            continue;
        }
        if (p < end && *p == '}')
        {
            p++;
            skip_space(p, end);
            if (p != end)
            {
                *error = "trailing text after the object";
                return false;
            }
            return true;
        }
        *error = "expected ',' or '}' after " + quoted(name);
        return false;
    }
}

}  // namespace table_options
}  // namespace tidesdb
