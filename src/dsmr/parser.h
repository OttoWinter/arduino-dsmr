/**
 * Arduino DSMR parser.
 *
 * This software is licensed under the MIT License.
 *
 * Copyright (c) 2015 Matthijs Kooijman <matthijs@stdin.nl>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Message parsing core
 */

#ifndef DSMR_INCLUDE_PARSER_H
#define DSMR_INCLUDE_PARSER_H

#include <util/crc16.h>
#include "util.h"

namespace dsmr {

/**
 * ParsedData is a template for the result of parsing a Dsmr P1 message.
 * You pass the fields you want to add to it as template arguments.
 *
 * This template will then generate a class that extends all the fields
 * passed (the fields really are classes themselves). Since each field
 * class has a single member variable, with the same name as the field
 * class, all of these fields will be available on the generated class.
 *
 * In other words, if I have:
 *
 * using MyData = ParsedData<
 *  identification,
 *  equipment_id
 * >;
 *
 * MyData data;
 *
 * then I can refer to the fields like data.identification and
 * data.equipment_id normally.
 *
 * Furthermore, this class offers some helper methods that can be used
 * to loop over all the fields inside it.
 */
template<typename... Ts>
struct ParsedData;

/**
 * Base case: No fields present.
 */
template<>
struct ParsedData<> {
  ParseResult<void> parse_line(const ObisId& id, const char *str, const char *end) {
  // Parsing succeeded, but found no matching handler (so return
  // set the next pointer to show nothing was parsed).
  return ParseResult<void>().until(str);
  }

  template<typename F>
  void applyEach(F f) {
  // Nothing to do
  }
};

/**
 * General case: At least one typename is passed.
 */
template<typename T, typename... Ts>
struct ParsedData<T, Ts...> : public T, ParsedData<Ts...> {
  /**
   * This method is used by the parser to parse a single line. The
   * OBIS id of the line is passed, and this method recursively finds a
   * field with a matching id. If any, it calls it's parse method, which
   * parses the value and stores it in the field.
   */
  ParseResult<void> parse_line(const ObisId& id, const char *str, const char *end) {
  if (id == T::id)
    return T::parse(str, end);
  return ParsedData<Ts...>::parse_line(id, str, end);
  }

  template<typename F>
  void applyEach(F f) {
  T::apply(f);
  return ParsedData<Ts...>::applyEach(f);
  }
};

struct StringParser {
  static ParseResult<String> parse_string(size_t min, size_t max, const char *str, const char *end) {
    ParseResult<String> res;
    if (str >= end || *str != '(')
      return res.fail(F("Missing ("), str);

    const char *str_start = str + 1; // Skip (
    const char *str_end = str_start;

    while(str_end < end && *str_end != ')')
      ++str_end;

    if (str_end == end)
      return res.fail(F("Missing )"), str);

    size_t len = str_end - str_start;
    if (len < min || len > max)
      return res.fail(F("Invalid string length"), str);

    concat_hack(res.result, str_start, len);

    return res.until(str_end + 1); // Skip )
  }
};

struct ObisIdParser {
  static ParseResult<ObisId> parse(const char *str, const char *end) {
    // Parse a Obis ID of the form 1-2:3.4.5.6
    // Stops parsing on the first unrecognized character. Any unparsed
    // parts are set to 255.
    ParseResult<ObisId> res;
    ObisId& id = res.result;
    res.next = str;
    uint8_t part = 0;
    while (res.next < end) {
      char c = *res.next;

      if (c >= '0' && c <= '9') {
        uint8_t digit = c - '0';
        if (id.v[part] > 25 || (id.v[part] == 25 && digit > 5))
          return res.fail(F("Obis ID has number over 255"), str);
        id.v[part] = id.v[part] * 10 + digit;
      } else if (part == 0 && c == '-') {
        part++;
      } else if (part == 1 && c == ':') {
        part++;
      } else if (part > 1 && part < 5 && c == '.') {
        part++;
      } else {
        break;
      }
      ++res.next;
    }

    if (res.next == str)
      return res.fail(F("OBIS id Empty"), str);

    for (++part; part < 6; ++part)
      id.v[part] = 255;

    return res;
  }
};

struct CrcParser {
  static const size_t CRC_LEN = 4;

  // Parse a crc value. str must point to the first of the four hex
  // bytes in the CRC.
  static ParseResult<uint16_t> parse(const char *str, const char *end) {
    ParseResult<uint16_t> res;
    // This should never happen with the code in this library, but
    // check anyway
    if (str + CRC_LEN > end)
      return res.fail(F("No checksum found"), str);

    // A bit of a messy way to parse the checksum, but all
    // integer-parse functions assume nul-termination
    char buf[CRC_LEN + 1];
    memcpy(buf, str, CRC_LEN);
    buf[CRC_LEN] = '\0';
    char *endp;
    uint16_t check = strtoul(buf, &endp, 16);

    // See if all four bytes formed a valid number
    if (endp != buf + CRC_LEN)
      return res.fail(F("Incomplete or malformed checksum"), str);

    res.next = str + CRC_LEN;
    return res.succeed(check);
  }
};

struct P1Parser {
  /**
    * Parse a complete P1 telegram. The string passed should start
    * with '/' and run up to and including the ! and the following
    * four byte checksum. It's ok if the string is longer, the .next
    * pointer in the result will indicate the next unprocessed byte.
    */
  template <typename... Ts>
  static ParseResult<void> parse(ParsedData<Ts...> *data, const char *str, size_t n) {
    ParseResult<void> res;
    if (!n || str[0] != '/')
      return res.fail(F("Data should start with /"), str);

    // Skip /
    const char *data_start = str + 1;

    // Look for ! that terminates the data
    const char *data_end = data_start;
    uint16_t crc = _crc16_update(0, *str); // Include the / in CRC
    while (data_end < str + n && *data_end != '!') {
      crc = _crc16_update(crc, *data_end);
      ++data_end;
    }

    if (data_end >= str + n)
      return res.fail(F("No checksum found"));

    crc = _crc16_update(crc, *data_end); // Include the ! in CRC

    ParseResult<uint16_t> check_res = CrcParser::parse(data_end + 1, str + n);
    if (check_res.err)
      return check_res;

    // Check CRC
    if (check_res.result != crc)
      return res.fail(F("Checksum mismatch"), data_end);

    res = parse_data(data, data_start, data_end);
    res.next = check_res.next;
    return res;
  }

  /**
   * Parse the data part of a message. Str should point to the first
   * character after the leading /, end should point to the ! before the
   * checksum. Does not verify the checksum.
   */
  template <typename... Ts>
  static ParseResult<void> parse_data(ParsedData<Ts...> *data, const char *str, const char *end) {
    ParseResult<void> res;
    // Split into lines and parse those
    const char *line_end = str, *line_start = str;

    // Parse ID line
    while (line_end < end) {
      if (*line_end == '\r' || *line_end == '\n') {
        // The first identification line looks like:
        // XXX5<id string>
        // The DSMR spec is vague on details, but in 62056-21, the X's
        // are a three-leter (registerd) manufacturer ID, the id
        // string is up to 16 chars of arbitrary characters and the
        // '5' is a baud rate indication. 5 apparently means 9600,
        // which DSMR 3.x and below used. It seems that DSMR 2.x
        // passed '3' here (which is mandatory for "mode D"
        // communication according to 62956-21), so we also allow
        // that.
        if (line_start + 3 >= line_end || (line_start[3] != '5' && line_start[3] != '3'))
          return res.fail(F("Invalid identification string"), line_start);
        // Offer it for processing using the all-ones Obis ID, which
        // is not otherwise valid.
        ParseResult<void> tmp = data->parse_line(ObisId(255, 255, 255, 255, 255, 255), line_start, line_end);
        if (tmp.err)
          return tmp;
        line_start = ++line_end;
        break;
      }
      ++line_end;
    }

    // Parse data lines
    while (line_end < end) {
      if (*line_end == '\r' || *line_end == '\n') {
        ParseResult<void> tmp = parse_line(data, line_start, line_end);
        if (tmp.err)
          return tmp;
        line_start = line_end + 1;
      }
      line_end++;
    }

    if (line_end != line_start)
      return res.fail(F("Last dataline not CRLF terminated"), line_start);

    return res;
  }

  template <typename Data>
  static ParseResult<void> parse_line(Data *data, const char *line, const char *end) {
    ParseResult<void> res;
    if (line == end)
      return res;

    ParseResult<ObisId> idres = ObisIdParser::parse(line, end);
    if (idres.err)
      return idres;

    ParseResult<void> datares = data->parse_line(idres.result, idres.next, end);
    if (datares.err)
      return datares;

    // If datares.next didn't move at all, there was no parser for
    // this field, that's ok. But if it did move, but not all the way
    // to the end, that's an error.
    if (datares.next != idres.next && datares.next != end)
      return res.fail(F("Trailing characters on data line"), datares.next);

    return res.until(end);
  }
};

} // namespace dsmr

#endif // DSMR_INCLUDE_PARSER_H
