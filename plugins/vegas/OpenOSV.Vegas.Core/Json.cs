// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Json.cs - a small, strict, never-throwing JSON reader and a pretty writer.
//
// Why not a library: the extension ships two DLLs and nothing else (no NuGet
// packages, no System.Web.Extensions dependency that some VEGAS installs lock
// down).  The documents it reads are osvtool's probe output and its own
// settings file, so a compact reader with defensive limits is all it needs.

using System;
using System.Collections.Generic;
using System.Globalization;
using System.Text;

namespace OpenOSV.Vegas.Core
{
    /// <summary>The kind of a <see cref="JsonValue"/>.</summary>
    public enum JsonKind
    {
        /// <summary>JSON <c>null</c>, or a value that is not there at all.</summary>
        Null,
        /// <summary><c>true</c> / <c>false</c>.</summary>
        Bool,
        /// <summary>A number.</summary>
        Number,
        /// <summary>A string.</summary>
        String,
        /// <summary>An array.</summary>
        Array,
        /// <summary>An object.</summary>
        Object,
    }

    /// <summary>
    /// One immutable JSON value.  Every accessor is total: asking a string for
    /// a key, or an object for element 7, gives <see cref="Missing"/> instead
    /// of throwing, so a chain like <c>doc["format"]["fps"].AsDouble(0)</c>
    /// reads a malformed document without a single null check.
    /// </summary>
    public sealed class JsonValue
    {
        /// <summary>The value of anything that is not in the document.</summary>
        public static readonly JsonValue Missing = new JsonValue(JsonKind.Null, null, 0, null, null, null, true);

        /// <summary>A present JSON <c>null</c>.</summary>
        public static readonly JsonValue NullValue = new JsonValue(JsonKind.Null, null, 0, null, null, null, false);

        private readonly string _text;
        private readonly double _number;
        private readonly string _rawNumber;
        private readonly List<JsonValue> _items;
        private readonly List<KeyValuePair<string, JsonValue>> _members;

        private JsonValue(JsonKind kind, string text, double number, string rawNumber, List<JsonValue> items,
                          List<KeyValuePair<string, JsonValue>> members, bool missing)
        {
            Kind = kind;
            _text = text;
            _number = number;
            _rawNumber = rawNumber;
            _items = items;
            _members = members;
            IsMissing = missing;
        }

        /// <summary>What this value is.</summary>
        public JsonKind Kind { get; }

        /// <summary>True for <see cref="Missing"/>: the key or index was not there.</summary>
        public bool IsMissing { get; }

        /// <summary>True when the value is present and not JSON null.</summary>
        public bool HasValue => !IsMissing && Kind != JsonKind.Null;

        /// <summary>True for an object.</summary>
        public bool IsObject => Kind == JsonKind.Object;

        /// <summary>True for an array.</summary>
        public bool IsArray => Kind == JsonKind.Array;

        /// <summary>Number of array elements or object members (0 otherwise).</summary>
        public int Count => _items != null ? _items.Count : (_members != null ? _members.Count : 0);

        /// <summary>An object member by key (last one wins), or <see cref="Missing"/>.</summary>
        public JsonValue this[string key]
        {
            get
            {
                if (_members == null || key == null)
                {
                    return Missing;
                }
                // Scan from the end: a duplicated key's last value wins, as in
                // most JSON readers.
                for (int i = _members.Count - 1; i >= 0; --i)
                {
                    if (string.Equals(_members[i].Key, key, StringComparison.Ordinal))
                    {
                        return _members[i].Value;
                    }
                }
                return Missing;
            }
        }

        /// <summary>An array element, or <see cref="Missing"/>.</summary>
        public JsonValue this[int index]
        {
            get
            {
                if (_items == null || index < 0 || index >= _items.Count)
                {
                    return Missing;
                }
                return _items[index];
            }
        }

        /// <summary>The array's elements (empty for anything else).</summary>
        public IEnumerable<JsonValue> Items
        {
            get
            {
                if (_items == null)
                {
                    yield break;
                }
                foreach (JsonValue v in _items)
                {
                    yield return v;
                }
            }
        }

        /// <summary>The object's members in document order (empty for anything else).</summary>
        public IEnumerable<KeyValuePair<string, JsonValue>> Members
        {
            get
            {
                if (_members == null)
                {
                    yield break;
                }
                foreach (KeyValuePair<string, JsonValue> m in _members)
                {
                    yield return m;
                }
            }
        }

        /// <summary>True when the object has the key.</summary>
        public bool Has(string key) => !this[key].IsMissing;

        /// <summary>The string, or <paramref name="fallback"/> for anything else.</summary>
        public string AsString(string fallback = null) => Kind == JsonKind.String ? _text : fallback;

        /// <summary>The number (or a numeric string), or <paramref name="fallback"/>.</summary>
        public double AsDouble(double fallback = 0.0)
        {
            if (Kind == JsonKind.Number && !double.IsNaN(_number) && !double.IsInfinity(_number))
            {
                return _number;
            }
            if (Kind == JsonKind.String && double.TryParse(_text, NumberStyles.Float, CultureInfo.InvariantCulture, out double parsed)
                && !double.IsNaN(parsed) && !double.IsInfinity(parsed))
            {
                return parsed;
            }
            return fallback;
        }

        /// <summary>
        /// The number as a 64-bit integer, or <paramref name="fallback"/>.  An
        /// integral literal is read from its digits, so a microsecond timestamp
        /// past 2^53 keeps every digit; a fractional number is rejected rather
        /// than silently truncated.
        /// </summary>
        public long AsLong(long fallback = 0)
        {
            if (Kind == JsonKind.Number)
            {
                if (_rawNumber != null && long.TryParse(_rawNumber, NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out long exact))
                {
                    return exact;
                }
                if (!double.IsNaN(_number) && Math.Abs(_number) < 9.2e18 && Math.Floor(_number) == _number)
                {
                    return (long)_number;
                }
                return fallback;
            }
            if (Kind == JsonKind.String && long.TryParse(_text, NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out long fromText))
            {
                return fromText;
            }
            return fallback;
        }

        /// <summary><see cref="AsLong"/> clamped into an <see cref="int"/>.</summary>
        public int AsInt(int fallback = 0)
        {
            long v = AsLong(long.MinValue);
            if (v == long.MinValue)
            {
                return fallback;
            }
            if (v > int.MaxValue || v < int.MinValue)
            {
                return fallback;
            }
            return (int)v;
        }

        /// <summary>The boolean (also 0/1 and "true"/"false"), or <paramref name="fallback"/>.</summary>
        public bool AsBool(bool fallback = false)
        {
            switch (Kind)
            {
                case JsonKind.Bool: return _number != 0.0;
                case JsonKind.Number: return _number != 0.0;
                case JsonKind.String:
                    if (string.Equals(_text, "true", StringComparison.OrdinalIgnoreCase)) return true;
                    if (string.Equals(_text, "false", StringComparison.OrdinalIgnoreCase)) return false;
                    return fallback;
                default: return fallback;
            }
        }

        // ---- construction (the parser's and the writer's) ---------------------

        internal static JsonValue FromString(string s) => new JsonValue(JsonKind.String, s ?? string.Empty, 0, null, null, null, false);

        internal static JsonValue FromBool(bool b) => new JsonValue(JsonKind.Bool, null, b ? 1.0 : 0.0, null, null, null, false);

        internal static JsonValue FromNumber(double d, string raw) => new JsonValue(JsonKind.Number, null, d, raw, null, null, false);

        internal static JsonValue FromArray(List<JsonValue> items) => new JsonValue(JsonKind.Array, null, 0, null, items ?? new List<JsonValue>(), null, false);

        internal static JsonValue FromObject(List<KeyValuePair<string, JsonValue>> members) =>
            new JsonValue(JsonKind.Object, null, 0, null, null, members ?? new List<KeyValuePair<string, JsonValue>>(), false);
    }

    /// <summary>
    /// The JSON reader.  Strict RFC 8259 grammar plus two tolerances that real
    /// files need: a UTF-8 byte order mark, and trailing text after the top
    /// level value (osvtool may print a summary line after the document).
    /// </summary>
    public static class Json
    {
        /// <summary>Deepest nesting accepted (osvtool's documents nest about 8 deep).</summary>
        public const int MaxDepth = 256;

        /// <summary>Largest document accepted, in characters.</summary>
        public const int MaxLength = 64 * 1024 * 1024;

        /// <summary>
        /// Parse <paramref name="text"/>.  Returns false with a one-line
        /// <paramref name="error"/> (never throws) on anything malformed.
        /// </summary>
        /// <param name="text">The document.</param>
        /// <param name="value">The parsed value, or <see cref="JsonValue.Missing"/>.</param>
        /// <param name="error">Why parsing failed, or null.</param>
        public static bool TryParse(string text, out JsonValue value, out string error)
        {
            value = JsonValue.Missing;
            error = null;
            if (text == null)
            {
                error = "no text";
                return false;
            }
            if (text.Length > MaxLength)
            {
                error = "document too large";
                return false;
            }
            try
            {
                var reader = new Reader(text);
                reader.SkipBomAndSpace();
                value = reader.ReadValue(0);
                return true;
            }
            catch (FormatException ex)
            {
                value = JsonValue.Missing;
                error = ex.Message;
                return false;
            }
            catch (Exception ex)
            {
                // Anything else (a stack or memory surprise) is still "not JSON".
                value = JsonValue.Missing;
                error = "unreadable JSON: " + ex.GetType().Name;
                return false;
            }
        }

        /// <summary>
        /// Find the first line of <paramref name="mixed"/> that starts a JSON
        /// object and parse from there.  For tools that print a human summary
        /// around the document on stdout.
        /// </summary>
        /// <param name="mixed">Console output.</param>
        /// <param name="value">The parsed object.</param>
        /// <param name="error">Why no object was found.</param>
        public static bool TryParseEmbeddedObject(string mixed, out JsonValue value, out string error)
        {
            value = JsonValue.Missing;
            error = null;
            if (string.IsNullOrEmpty(mixed))
            {
                error = "no output";
                return false;
            }
            // A '{' at the start of the text or right after a newline (with
            // optional indentation) is the start of a document.
            int start = -1;
            bool lineStart = true;
            for (int i = 0; i < mixed.Length; ++i)
            {
                char c = mixed[i];
                if (c == '\n')
                {
                    lineStart = true;
                    continue;
                }
                if (lineStart && (c == ' ' || c == '\t' || c == '\r' || c == (char)0xFEFF))
                {
                    continue;
                }
                if (lineStart && c == '{')
                {
                    start = i;
                    break;
                }
                lineStart = false;
            }
            if (start < 0)
            {
                error = "no JSON object in the output";
                return false;
            }
            if (!TryParse(mixed.Substring(start), out value, out error))
            {
                return false;
            }
            if (!value.IsObject)
            {
                error = "the output's JSON is not an object";
                value = JsonValue.Missing;
                return false;
            }
            return true;
        }

        /// <summary>The recursive-descent reader over one string.</summary>
        private sealed class Reader
        {
            private readonly string _s;
            private int _pos;

            public Reader(string s)
            {
                _s = s;
                _pos = 0;
            }

            public void SkipBomAndSpace()
            {
                if (_pos < _s.Length && _s[_pos] == (char)0xFEFF)
                {
                    ++_pos;
                }
                SkipSpace();
            }

            private void SkipSpace()
            {
                while (_pos < _s.Length)
                {
                    char c = _s[_pos];
                    if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
                    {
                        ++_pos;
                    }
                    else
                    {
                        break;
                    }
                }
            }

            private FormatException Fail(string what) =>
                new FormatException(what + " at character " + _pos.ToString(CultureInfo.InvariantCulture));

            public JsonValue ReadValue(int depth)
            {
                if (depth > MaxDepth)
                {
                    throw Fail("JSON nested too deeply");
                }
                SkipSpace();
                if (_pos >= _s.Length)
                {
                    throw Fail("unexpected end of JSON");
                }
                char c = _s[_pos];
                switch (c)
                {
                    case '{': return ReadObject(depth);
                    case '[': return ReadArray(depth);
                    case '"': return JsonValue.FromString(ReadString());
                    case 't': ExpectWord("true"); return JsonValue.FromBool(true);
                    case 'f': ExpectWord("false"); return JsonValue.FromBool(false);
                    case 'n': ExpectWord("null"); return JsonValue.NullValue;
                    default:
                        if (c == '-' || (c >= '0' && c <= '9'))
                        {
                            return ReadNumber();
                        }
                        throw Fail("unexpected character '" + c + "'");
                }
            }

            private void ExpectWord(string word)
            {
                if (_pos + word.Length > _s.Length || string.CompareOrdinal(_s, _pos, word, 0, word.Length) != 0)
                {
                    throw Fail("expected '" + word + "'");
                }
                _pos += word.Length;
            }

            private JsonValue ReadObject(int depth)
            {
                ++_pos; // '{'
                var members = new List<KeyValuePair<string, JsonValue>>();
                SkipSpace();
                if (_pos < _s.Length && _s[_pos] == '}')
                {
                    ++_pos;
                    return JsonValue.FromObject(members);
                }
                while (true)
                {
                    SkipSpace();
                    if (_pos >= _s.Length || _s[_pos] != '"')
                    {
                        throw Fail("expected a member name");
                    }
                    string key = ReadString();
                    SkipSpace();
                    if (_pos >= _s.Length || _s[_pos] != ':')
                    {
                        throw Fail("expected ':'");
                    }
                    ++_pos;
                    JsonValue v = ReadValue(depth + 1);
                    members.Add(new KeyValuePair<string, JsonValue>(key, v));
                    SkipSpace();
                    if (_pos >= _s.Length)
                    {
                        throw Fail("unterminated object");
                    }
                    if (_s[_pos] == ',')
                    {
                        ++_pos;
                        continue;
                    }
                    if (_s[_pos] == '}')
                    {
                        ++_pos;
                        return JsonValue.FromObject(members);
                    }
                    throw Fail("expected ',' or '}'");
                }
            }

            private JsonValue ReadArray(int depth)
            {
                ++_pos; // '['
                var items = new List<JsonValue>();
                SkipSpace();
                if (_pos < _s.Length && _s[_pos] == ']')
                {
                    ++_pos;
                    return JsonValue.FromArray(items);
                }
                while (true)
                {
                    items.Add(ReadValue(depth + 1));
                    SkipSpace();
                    if (_pos >= _s.Length)
                    {
                        throw Fail("unterminated array");
                    }
                    if (_s[_pos] == ',')
                    {
                        ++_pos;
                        continue;
                    }
                    if (_s[_pos] == ']')
                    {
                        ++_pos;
                        return JsonValue.FromArray(items);
                    }
                    throw Fail("expected ',' or ']'");
                }
            }

            private string ReadString()
            {
                ++_pos; // opening quote
                var sb = new StringBuilder();
                while (true)
                {
                    if (_pos >= _s.Length)
                    {
                        throw Fail("unterminated string");
                    }
                    char c = _s[_pos++];
                    if (c == '"')
                    {
                        return sb.ToString();
                    }
                    if (c == '\\')
                    {
                        if (_pos >= _s.Length)
                        {
                            throw Fail("unterminated escape");
                        }
                        char e = _s[_pos++];
                        switch (e)
                        {
                            case '"': sb.Append('"'); break;
                            case '\\': sb.Append('\\'); break;
                            case '/': sb.Append('/'); break;
                            case 'b': sb.Append('\b'); break;
                            case 'f': sb.Append('\f'); break;
                            case 'n': sb.Append('\n'); break;
                            case 'r': sb.Append('\r'); break;
                            case 't': sb.Append('\t'); break;
                            case 'u':
                                if (_pos + 4 > _s.Length)
                                {
                                    throw Fail("short \\u escape");
                                }
                                if (!int.TryParse(_s.Substring(_pos, 4), NumberStyles.AllowHexSpecifier, CultureInfo.InvariantCulture, out int code))
                                {
                                    throw Fail("bad \\u escape");
                                }
                                sb.Append((char)code);
                                _pos += 4;
                                break;
                            default:
                                throw Fail("bad escape '\\" + e + "'");
                        }
                        continue;
                    }
                    if (c < 0x20)
                    {
                        throw Fail("control character in string");
                    }
                    sb.Append(c);
                }
            }

            private JsonValue ReadNumber()
            {
                int start = _pos;
                if (_s[_pos] == '-')
                {
                    ++_pos;
                }
                bool integral = true;
                while (_pos < _s.Length)
                {
                    char c = _s[_pos];
                    if (c >= '0' && c <= '9')
                    {
                        ++_pos;
                    }
                    else if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-')
                    {
                        integral = false;
                        ++_pos;
                    }
                    else
                    {
                        break;
                    }
                }
                string raw = _s.Substring(start, _pos - start);
                if (!double.TryParse(raw, NumberStyles.Float, CultureInfo.InvariantCulture, out double d))
                {
                    throw Fail("bad number '" + raw + "'");
                }
                return JsonValue.FromNumber(d, integral ? raw : null);
            }
        }
    }

    /// <summary>
    /// A tiny pretty-printing JSON writer, for the settings file.  Keys and
    /// strings are escaped per RFC 8259; doubles are written round-trip with
    /// the invariant culture, so a German-locale VEGAS writes "0.5", not "0,5".
    /// </summary>
    public sealed class JsonWriter
    {
        private readonly StringBuilder _sb = new StringBuilder();
        private readonly Stack<bool> _first = new Stack<bool>();
        private int _indent;

        /// <summary>The text written so far.</summary>
        public override string ToString() => _sb.ToString();

        /// <summary>Open an object (as a member when <paramref name="key"/> is given).</summary>
        public JsonWriter BeginObject(string key = null)
        {
            Prefix(key);
            _sb.Append('{');
            _first.Push(true);
            ++_indent;
            return this;
        }

        /// <summary>Close the innermost object.</summary>
        public JsonWriter EndObject()
        {
            --_indent;
            bool empty = _first.Count > 0 && _first.Pop();
            if (!empty)
            {
                NewLine();
            }
            _sb.Append('}');
            return this;
        }

        /// <summary>Open an array (as a member when <paramref name="key"/> is given).</summary>
        public JsonWriter BeginArray(string key = null)
        {
            Prefix(key);
            _sb.Append('[');
            _first.Push(true);
            ++_indent;
            return this;
        }

        /// <summary>Close the innermost array.</summary>
        public JsonWriter EndArray()
        {
            --_indent;
            bool empty = _first.Count > 0 && _first.Pop();
            if (!empty)
            {
                NewLine();
            }
            _sb.Append(']');
            return this;
        }

        /// <summary>A string member / element (null writes JSON null).</summary>
        public JsonWriter Value(string key, string value)
        {
            Prefix(key);
            if (value == null)
            {
                _sb.Append("null");
            }
            else
            {
                AppendQuoted(_sb, value);
            }
            return this;
        }

        /// <summary>A boolean member / element.</summary>
        public JsonWriter Value(string key, bool value)
        {
            Prefix(key);
            _sb.Append(value ? "true" : "false");
            return this;
        }

        /// <summary>An integer member / element.</summary>
        public JsonWriter Value(string key, long value)
        {
            Prefix(key);
            _sb.Append(value.ToString(CultureInfo.InvariantCulture));
            return this;
        }

        /// <summary>A number member / element (NaN and infinities become null).</summary>
        public JsonWriter Value(string key, double value)
        {
            Prefix(key);
            if (double.IsNaN(value) || double.IsInfinity(value))
            {
                _sb.Append("null");
            }
            else
            {
                _sb.Append(value.ToString("R", CultureInfo.InvariantCulture));
            }
            return this;
        }

        private void Prefix(string key)
        {
            if (_first.Count > 0)
            {
                bool first = _first.Pop();
                if (!first)
                {
                    _sb.Append(',');
                }
                _first.Push(false);
                NewLine();
            }
            if (key != null)
            {
                AppendQuoted(_sb, key);
                _sb.Append(": ");
            }
        }

        private void NewLine()
        {
            _sb.Append('\n');
            _sb.Append(' ', Math.Max(0, _indent) * 2);
        }

        /// <summary>Append <paramref name="s"/> as a quoted, escaped JSON string.</summary>
        public static void AppendQuoted(StringBuilder sb, string s)
        {
            sb.Append('"');
            foreach (char c in s)
            {
                switch (c)
                {
                    case '"': sb.Append("\\\""); break;
                    case '\\': sb.Append("\\\\"); break;
                    case '\n': sb.Append("\\n"); break;
                    case '\r': sb.Append("\\r"); break;
                    case '\t': sb.Append("\\t"); break;
                    case '\b': sb.Append("\\b"); break;
                    case '\f': sb.Append("\\f"); break;
                    default:
                        if (c < 0x20 || c == (char)0x2028 || c == (char)0x2029)
                        {
                            sb.Append("\\u").Append(((int)c).ToString("x4", CultureInfo.InvariantCulture));
                        }
                        else
                        {
                            sb.Append(c);
                        }
                        break;
                }
            }
            sb.Append('"');
        }
    }
}
