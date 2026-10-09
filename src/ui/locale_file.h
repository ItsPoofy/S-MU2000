// license:BSD-3-Clause
//
// Reads a translation file: one JSON object of "key": "text" pairs, where
// comments (// to the end of the line, /* ... */) are allowed anywhere outside
// a string so that translators can be left notes. This is the same shape as
// the files under locale/ in the repository (tools/locale_tool.py builds the
// program's tables from those); here the program reads the ones a user has put
// in the settings folder, to try a translation without building anything.
// See src/ui/texts.h for where they are looked up and locale/README.md for
// the translator's side.
//
// Header-only, no dependencies beyond the standard library.

#ifndef S_MU2000_UI_LOCALE_FILE_H
#define S_MU2000_UI_LOCALE_FILE_H

#pragma once

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace ui::locale_file {

namespace detail {

struct reader {
	const std::string &s;
	size_t i = 0;
	int line = 1;

	// Skips blanks and comments. False at the end of the text
	bool skip()
	{
		while (i < s.size()) {
			const char c = s[i];
			if (c == '\n') {
				line++;
				i++;
			} else if (c == ' ' || c == '\t' || c == '\r') {
				i++;
			} else if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') {
				while (i < s.size() && s[i] != '\n')
					i++;
			} else if (c == '/' && i + 1 < s.size() && s[i + 1] == '*') {
				i += 2;
				while (i + 1 < s.size() && !(s[i] == '*' && s[i + 1] == '/')) {
					if (s[i] == '\n')
						line++;
					i++;
				}
				i = i + 1 < s.size() ? i + 2 : s.size();
			} else {
				return true;
			}
		}
		return false;
	}

	static void utf8(std::string &out, unsigned cp)
	{
		if (cp < 0x80) {
			out += char(cp);
		} else if (cp < 0x800) {
			out += char(0xc0 | (cp >> 6));
			out += char(0x80 | (cp & 0x3f));
		} else if (cp < 0x10000) {
			out += char(0xe0 | (cp >> 12));
			out += char(0x80 | ((cp >> 6) & 0x3f));
			out += char(0x80 | (cp & 0x3f));
		} else {
			out += char(0xf0 | (cp >> 18));
			out += char(0x80 | ((cp >> 12) & 0x3f));
			out += char(0x80 | ((cp >> 6) & 0x3f));
			out += char(0x80 | (cp & 0x3f));
		}
	}

	bool hex4(unsigned &v)
	{
		if (i + 4 > s.size())
			return false;
		v = 0;
		for (int k = 0; k < 4; k++) {
			const char c = s[i++];
			v <<= 4;
			if (c >= '0' && c <= '9')
				v |= unsigned(c - '0');
			else if (c >= 'a' && c <= 'f')
				v |= unsigned(c - 'a' + 10);
			else if (c >= 'A' && c <= 'F')
				v |= unsigned(c - 'A' + 10);
			else
				return false;
		}
		return true;
	}

	// A JSON string, the opening quote being at s[i]
	bool string(std::string &out)
	{
		if (i >= s.size() || s[i] != '"')
			return false;
		i++;
		out.clear();
		while (i < s.size()) {
			const char c = s[i++];
			if (c == '"')
				return true;
			if (c == '\n')
				return false;                 // a string does not run over a line
			if (c != '\\') {
				out += c;
				continue;
			}
			if (i >= s.size())
				return false;
			const char e = s[i++];
			switch (e) {
			case '"': out += '"'; break;
			case '\\': out += '\\'; break;
			case '/': out += '/'; break;
			case 'n': out += '\n'; break;
			case 't': out += '\t'; break;
			case 'r': break;                  // the tables hold \n only
			case 'b': case 'f': break;
			case 'u': {
				unsigned cp = 0;
				if (!hex4(cp))
					return false;
				if (cp >= 0xd800 && cp < 0xdc00 && i + 1 < s.size() && s[i] == '\\' && s[i + 1] == 'u') {
					i += 2;
					unsigned lo = 0;
					if (!hex4(lo) || lo < 0xdc00 || lo > 0xdfff)
						return false;
					cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
				}
				if (cp == 0)
					return false;             // no NUL inside a C string
				utf8(out, cp);
				break;
			}
			default:
				return false;
			}
		}
		return false;
	}
};

} // namespace detail

// The pairs of one file, in file order. On a syntax error returns false with
// the line number in `line`; `out` then holds what was read before it.
inline bool parse(const std::string &text, std::vector<std::pair<std::string, std::string>> &out, int &line)
{
	// A UTF-8 byte order mark at the start is what Notepad writes; step over it
	static const std::string BOM = "\xef\xbb\xbf";
	const std::string body = text.compare(0, BOM.size(), BOM) == 0 ? text.substr(BOM.size()) : text;
	detail::reader r{ body };
	const auto fail = [&] {
		line = r.line;
		return false;
	};
	if (!r.skip() || body[r.i] != '{')
		return fail();
	r.i++;
	for (;;) {
		if (!r.skip())
			return fail();
		if (body[r.i] == '}')
			break;
		std::string key, value;
		if (!r.string(key))
			return fail();
		if (!r.skip() || body[r.i] != ':')
			return fail();
		r.i++;
		if (!r.skip() || !r.string(value))
			return fail();
		out.emplace_back(std::move(key), std::move(value));
		if (!r.skip())
			return fail();
		if (body[r.i] == ',') {
			r.i++;                            // a comma before the closing brace is let through
			continue;
		}
		if (body[r.i] != '}')
			return fail();
		break;
	}
	r.i++;
	if (r.skip())
		return fail();                        // something after the object
	line = 0;
	return true;
}

// The whole file, or false. Refuses anything over 4 MB (a translation is a few
// hundred kilobytes)
inline bool read(const std::string &path, std::string &text)
{
	std::FILE *f = std::fopen(path.c_str(), "rb");
	if (!f)
		return false;
	text.clear();
	char buf[16384];
	size_t n;
	bool ok = true;
	while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
		text.append(buf, n);
		if (text.size() > (4u << 20)) {
			ok = false;
			break;
		}
	}
	std::fclose(f);
	return ok;
}

// The printf conversions of a text, in order ("%d%s" for "%d of %s"; "%%" is
// not one). Two texts are safe to swap only when these are equal: a translation
// that turns %s into %d would crash the program that formats it.
inline std::string printf_shape(const char *s)
{
	std::string out;
	for (const char *p = s; *p; p++) {
		if (*p != '%')
			continue;
		const char *q = p + 1;
		if (*q == '%') {
			p = q;
			continue;
		}
		// flags, width and precision may differ between languages; the length
		// and the conversion letter may not
		while (*q && std::char_traits<char>::find("-+ #'0123456789.", 16, *q))
			q++;
		const char *len = q;
		while (*q && std::char_traits<char>::find("hljztL", 6, *q))
			q++;
		if (!*q) {
			out += "%?";                      // a lone % at the end: never equal to a real one
			break;
		}
		out += '%';
		out.append(len, size_t(q - len) + 1);
		p = q;
	}
	return out;
}

} // namespace ui::locale_file

#endif // S_MU2000_UI_LOCALE_FILE_H
