/*
* Copyright 2016 Huy Cuong Nguyen
* Copyright 2016 ZXing authors
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*      http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/

#include "datamatrix/DMHighLevelEncoder.h"
#include "datamatrix/DMEncoderContext.h"
#include "TextEncoder.h"
#include "CharacterSet.h"
#include "ZXStrConvWorkaround.h"

#include <string>
#include <algorithm>
#include <array>
#include <limits>
#include <cmath>
#include <numeric>
#include <functional>
#include <stdexcept>

namespace ZXing {
namespace DataMatrix {

static const int PAD = 129;
static const int UPPER_SHIFT = 235;
static const int MACRO_05 = 236;
static const int MACRO_06 = 237;
static const int C40_UNLATCH = 254;
static const int X12_UNLATCH = 254;

static const std::wstring MACRO_05_HEADER = L"[)>\x1E05\x1D";
static const std::wstring MACRO_06_HEADER = L"[)>\x1E06\x1D";
static const std::wstring MACRO_TRAILER = L"\x1E\x04";

enum
{
	ASCII_ENCODATION,
	C40_ENCODATION,
	TEXT_ENCODATION,
	X12_ENCODATION,
	EDIFACT_ENCODATION,
	BASE256_ENCODATION,
};

static const int LATCHES[] = {
	0,	// ASCII mode, no latch needed
	230, // LATCH_TO_C40
	239, // LATCH_TO_TEXT
	238, // LATCH_TO_ANSIX12
	240, // LATCH_TO_EDIFACT
	231, // LATCH_TO_BASE256,
};

static inline bool IsDigit(int ch)
{
	return ch >= '0' && ch <= '9';
}

static inline bool IsExtendedASCII(int ch)
{
	return ch >= 128 && ch <= 255;
}

static inline bool IsNativeC40(int ch)
{
	return (ch == ' ') || (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z');
}

static inline bool IsNativeText(int ch)
{
	return (ch == ' ') || (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z');
}

static inline bool IsX12TermSep(int ch)
{
	return (ch == '\r') //CR
		|| (ch == '*')
		|| (ch == '>');
}

static inline bool IsNativeX12(int ch)
{
	return IsX12TermSep(ch) || (ch == ' ') || (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z');
}

static inline bool IsNativeEDIFACT(int ch)
{
	return ch >= ' ' && ch <= '^';
}

static inline bool IsSpecialB256(int ch)
{
	return false; //TODO NOT IMPLEMENTED YET!!!
}


/*
* Converts the message to a byte array using the default encoding (cp437) as defined by the
* specification
*
* @param msg the message
* @return the byte array of the message
*/

/*
public static byte[] getBytesForMessage(String msg) {
return msg.getBytes(Charset.forName("cp437")); //See 4.4.3 and annex B of ISO/IEC 15438:2001(E)
}
*/

static int Randomize253State(int ch, int codewordPosition)
{
	int pseudoRandom = ((149 * codewordPosition) % 253) + 1;
	int tempVariable = ch + pseudoRandom;
	return tempVariable <= 254 ? tempVariable : (tempVariable - 254);
}

static int FindMinimums(const std::array<int, 6>& intCharCounts, int min, std::array<int, 6>& mins)
{
	mins.fill(0);
	for (int i = 0; i < 6; i++) {
		int current = intCharCounts[i];
		if (min > current) {
			min = current;
			mins.fill(0);
		}
		if (min == current) {
			mins[i]++;
		}
	}
	return min;
}

static int LookAheadTest(const std::string& msg, size_t startpos, int currentMode)
{
	if (startpos >= msg.length()) {
		return currentMode;
	}
	std::array<float, 6> charCounts = { 0.5f, 1, 1, 1, 1, 1.25f };
	//step J
	if (currentMode != ASCII_ENCODATION) {
		std::transform(charCounts.begin(), charCounts.end(), charCounts.begin(), [](float x) { return x * 2; });
	}
	charCounts[currentMode] = 0;

	std::array<int, 6> mins;
	std::array<int, 6> intCharCounts;
	int charsProcessed = 0;
	while (true) {
		//step K
		if ((startpos + charsProcessed) == msg.length()) {
			int min = std::numeric_limits<int>::max();
			std::transform(charCounts.begin(), charCounts.end(), intCharCounts.begin(), [](float x) { return static_cast<int>(std::ceil(x)); });
			min = FindMinimums(intCharCounts, min, mins);
			int minCount = std::accumulate(mins.begin(), mins.end(), 0);

			if (intCharCounts[ASCII_ENCODATION] == min) {
				return ASCII_ENCODATION;
			}
			if (minCount == 1 && mins[BASE256_ENCODATION] > 0) {
				return BASE256_ENCODATION;
			}
			if (minCount == 1 && mins[EDIFACT_ENCODATION] > 0) {
				return EDIFACT_ENCODATION;
			}
			if (minCount == 1 && mins[TEXT_ENCODATION] > 0) {
				return TEXT_ENCODATION;
			}
			if (minCount == 1 && mins[X12_ENCODATION] > 0) {
				return X12_ENCODATION;
			}
			return C40_ENCODATION;
		}

		int c = msg.at(startpos + charsProcessed);
		charsProcessed++;

		//step L
		if (IsDigit(c)) {
			charCounts[ASCII_ENCODATION] += 0.5f;
		}
		else if (IsExtendedASCII(c)) {
			charCounts[ASCII_ENCODATION] = std::ceil(charCounts[ASCII_ENCODATION]);
			charCounts[ASCII_ENCODATION] += 2.0f;
		}
		else {
			charCounts[ASCII_ENCODATION] = std::ceil(charCounts[ASCII_ENCODATION]);
			charCounts[ASCII_ENCODATION] += 1.0f;
		}

		//step M
		if (IsNativeC40(c)) {
			charCounts[C40_ENCODATION] += 2.0f / 3.0f;
		}
		else if (IsExtendedASCII(c)) {
			charCounts[C40_ENCODATION] += 8.0f / 3.0f;
		}
		else {
			charCounts[C40_ENCODATION] += 4.0f / 3.0f;
		}

		//step N
		if (IsNativeText(c)) {
			charCounts[TEXT_ENCODATION] += 2.0f / 3.0f;
		}
		else if (IsExtendedASCII(c)) {
			charCounts[TEXT_ENCODATION] += 8.0f / 3.0f;
		}
		else {
			charCounts[TEXT_ENCODATION] += 4.0f / 3.0f;
		}

		//step O
		if (IsNativeX12(c)) {
			charCounts[X12_ENCODATION] += 2.0f / 3.0f;
		}
		else if (IsExtendedASCII(c)) {
			charCounts[X12_ENCODATION] += 13.0f / 3.0f;
		}
		else {
			charCounts[X12_ENCODATION] += 10.0f / 3.0f;
		}

		//step P
		if (IsNativeEDIFACT(c)) {
			charCounts[EDIFACT_ENCODATION] += 3.0f / 4.0f;
		}
		else if (IsExtendedASCII(c)) {
			charCounts[EDIFACT_ENCODATION] += 17.0f / 4.0f;
		}
		else {
			charCounts[EDIFACT_ENCODATION] += 13.0f / 4.0f;
		}

		// step Q
		if (IsSpecialB256(c)) {
			charCounts[BASE256_ENCODATION] += 4.0f;
		}
		else {
			charCounts[BASE256_ENCODATION] += 1.0f;
		}

		//step R
		if (charsProcessed >= 4) {
			std::transform(charCounts.begin(), charCounts.end(), intCharCounts.begin(), [](float x) { return static_cast<int>(std::ceil(x)); });
			FindMinimums(intCharCounts, std::numeric_limits<int>::max(), mins);
			int minCount = std::accumulate(mins.begin(), mins.end(), 0);

			if (intCharCounts[ASCII_ENCODATION] < intCharCounts[BASE256_ENCODATION]
				&& intCharCounts[ASCII_ENCODATION] < intCharCounts[C40_ENCODATION]
				&& intCharCounts[ASCII_ENCODATION] < intCharCounts[TEXT_ENCODATION]
				&& intCharCounts[ASCII_ENCODATION] < intCharCounts[X12_ENCODATION]
				&& intCharCounts[ASCII_ENCODATION] < intCharCounts[EDIFACT_ENCODATION]) {
				return ASCII_ENCODATION;
			}
			if (intCharCounts[BASE256_ENCODATION] < intCharCounts[ASCII_ENCODATION]
				|| (mins[C40_ENCODATION] + mins[TEXT_ENCODATION] + mins[X12_ENCODATION] + mins[EDIFACT_ENCODATION]) == 0) {
				return BASE256_ENCODATION;
			}
			if (minCount == 1 && mins[EDIFACT_ENCODATION] > 0) {
				return EDIFACT_ENCODATION;
			}
			if (minCount == 1 && mins[TEXT_ENCODATION] > 0) {
				return TEXT_ENCODATION;
			}
			if (minCount == 1 && mins[X12_ENCODATION] > 0) {
				return X12_ENCODATION;
			}
			if (intCharCounts[C40_ENCODATION] + 1 < intCharCounts[ASCII_ENCODATION]
				&& intCharCounts[C40_ENCODATION] + 1 < intCharCounts[BASE256_ENCODATION]
				&& intCharCounts[C40_ENCODATION] + 1 < intCharCounts[EDIFACT_ENCODATION]
				&& intCharCounts[C40_ENCODATION] + 1 < intCharCounts[TEXT_ENCODATION]) {
				if (intCharCounts[C40_ENCODATION] < intCharCounts[X12_ENCODATION]) {
					return C40_ENCODATION;
				}
				if (intCharCounts[C40_ENCODATION] == intCharCounts[X12_ENCODATION]) {
					size_t p = startpos + charsProcessed + 1;
					while (p < msg.length()) {
						int tc = msg.at(p);
						if (IsX12TermSep(tc)) {
							return X12_ENCODATION;
						}
						if (!IsNativeX12(tc)) {
							break;
						}
						p++;
					}
					return C40_ENCODATION;
				}
			}
		}
	}
}

static std::string ToHexString(int c)
{
	const char* digits = "0123456789abcdef";
	std::string val(4, '0');
	val[1] = 'x';
	val[2] = digits[(c >> 4) & 0xf];
	val[3] = digits[c & 0xf];
	return val;
}

namespace ASCIIEncoder {
	/**
	* Determines the number of consecutive characters that are encodable using numeric compaction.
	*
	* @param msg      the message
	* @param startpos the start position within the message
	* @return the requested character count
	*/
	static int DetermineConsecutiveDigitCount(const std::string& msg, int startpos)
	{
		auto begin = msg.begin() + startpos;
		return static_cast<int>(std::find_if_not(begin, msg.end(), IsDigit) - begin);
	}

	static int EncodeASCIIDigits(int digit1, int digit2)
	{
		if (IsDigit(digit1) && IsDigit(digit2)) {
			int num = (digit1 - '0') * 10 + (digit2 - '0');
			return num + 130;
		}
		return '?';
	}

	static void EncodeASCII(EncoderContext& context)
	{
		//step B
		int n = DetermineConsecutiveDigitCount(context.message(), context.currentPos());
		if (n >= 2) {
			context.addCodeword(EncodeASCIIDigits(context.currentChar(), context.nextChar()));
			context.setCurrentPos(context.currentPos() + 2);
		}
		else {
			int c = context.currentChar();
			int newMode = LookAheadTest(context.message(), context.currentPos(), ASCII_ENCODATION);
			if (newMode != ASCII_ENCODATION)
			{
				// the order here is the same as ENCODATION;
				context.addCodeword(LATCHES[newMode]);
				context.setNewEncoding(newMode);
			}
			else if (IsExtendedASCII(c)) {
				context.addCodeword(UPPER_SHIFT);
				context.addCodeword(c - 128 + 1);
				context.setCurrentPos(context.currentPos() + 1);
			}
			else {
				context.addCodeword(c + 1);
				context.setCurrentPos(context.currentPos() + 1);
			}
		}
	}

} // ASCIIEncoder

namespace C40Encoder {

	static int EncodeChar(char c, std::string& sb)
	{
		if (c == ' ') {
			sb.push_back('\3');
			return 1;
		}
		if (c >= '0' && c <= '9') {
			sb.push_back((char)(c - 48 + 4));
			return 1;
		}
		if (c >= 'A' && c <= 'Z') {
			sb.push_back((char)(c - 65 + 14));
			return 1;
		}
		if (c >= '\0' && c <= '\x1f') {
			sb.push_back('\0'); //Shift 1 Set
			sb.push_back(c);
			return 2;
		}
		if (c >= '!' && c <= '/') {
			sb.push_back('\1'); //Shift 2 Set
			sb.push_back((char)(c - 33));
			return 2;
		}
		if (c >= ':' && c <= '@') {
			sb.push_back('\1'); //Shift 2 Set
			sb.push_back((char)(c - 58 + 15));
			return 2;
		}
		if (c >= '[' && c <= '_') {
			sb.push_back('\1'); //Shift 2 Set
			sb.push_back((char)(c - 91 + 22));
			return 2;
		}
		if (c >= '\x60' && c <= '\x7f') {
			sb.push_back('\2'); //Shift 3 Set
			sb.push_back((char)(c - 96));
			return 2;
		}
		if (c >= '\x80') {
			sb.append("\1\x1e"); //Shift 2, Upper Shift
			int len = 2;
			len += EncodeChar((char)(c - 0x80), sb);
			return len;
		}
		throw std::invalid_argument("Illegal character: " + ToHexString(c));
	}

	static int BacktrackOneCharacter(EncoderContext& context, std::string& buffer, std::string& removed, int lastCharSize)
	{
		int count = static_cast<int>(buffer.length());
		buffer.resize(buffer.size() - lastCharSize);
		context.setCurrentPos(context.currentPos() - 1);
		int c = context.currentChar();
		lastCharSize = EncodeChar(c, removed);
		context.resetSymbolInfo(); //Deal with possible reduction in symbol size
		return lastCharSize;
	}

	static void EncodeToCodewords(EncoderContext& context, const std::string& sb, int startPos) {
		int c1 = sb.at(startPos);
		int c2 = sb.at(startPos + 1);
		int c3 = sb.at(startPos + 2);
		int v = (1600 * c1) + (40 * c2) + c3 + 1;
		context.addCodeword(v / 256);
		context.addCodeword(v % 256);
	}

	static void WriteNextTriplet(EncoderContext& context, std::string& buffer)
	{
		EncodeToCodewords(context, buffer, 0);
		buffer.erase(0, 3);
	}

	/**
	* Handle "end of data" situations
	*
	* @param context the encoder context
	* @param buffer  the buffer with the remaining encoded characters
	*/
	static void HandleEOD(EncoderContext& context, std::string& buffer)
	{
		int unwritten = (static_cast<int>(buffer.length()) / 3) * 2;
		int rest = static_cast<int>(buffer.length()) % 3;

		int curCodewordCount = context.codewordCount() + unwritten;
		auto symbolInfo = context.updateSymbolInfo(curCodewordCount);
		int available = symbolInfo->dataCapacity() - curCodewordCount;

		if (rest == 2) {
			buffer.push_back('\0'); //Shift 1
			while (buffer.length() >= 3) {
				WriteNextTriplet(context, buffer);
			}
			if (context.hasMoreCharacters()) {
				context.addCodeword(C40_UNLATCH);
			}
		}
		else if (available == 1 && rest == 1) {
			while (buffer.length() >= 3) {
				WriteNextTriplet(context, buffer);
			}
			if (context.hasMoreCharacters()) {
				context.addCodeword(C40_UNLATCH);
			}
			// else no unlatch
			context.setCurrentPos(context.currentPos() - 1);
		}
		else if (rest == 0) {
			while (buffer.length() >= 3) {
				WriteNextTriplet(context, buffer);
			}
			if (available > 0 || context.hasMoreCharacters()) {
				context.addCodeword(C40_UNLATCH);
			}
		}
		else {
			throw std::logic_error("Unexpected case. Please report!");
		}
		context.setNewEncoding(ASCII_ENCODATION);
	}

	static void EncodeC40(EncoderContext& context, std::function<int (int, std::string&)> encodeChar, int encodingMode)
	{
		//step C
		std::string buffer;
		while (context.hasMoreCharacters()) {
			int c = context.currentChar();
			context.setCurrentPos(context.currentPos() + 1);
			int lastCharSize = encodeChar(c, buffer);
			int unwritten = static_cast<int>(buffer.length() / 3) * 2;
			int curCodewordCount = context.codewordCount() + unwritten;
			auto symbolInfo = context.updateSymbolInfo(curCodewordCount);
			int available = symbolInfo->dataCapacity() - curCodewordCount;

			if (!context.hasMoreCharacters()) {
				//Avoid having a single C40 value in the last triplet
				std::string removed;
				if ((buffer.length() % 3) == 2) {
					if (available < 2 || available > 2) {
						lastCharSize = BacktrackOneCharacter(context, buffer, removed, lastCharSize);
					}
				}
				while ((buffer.length() % 3) == 1 && ((lastCharSize <= 3 && available != 1) || lastCharSize > 3)) {
					lastCharSize = BacktrackOneCharacter(context, buffer, removed, lastCharSize);
				}
				break;
			}

			if ((buffer.length() % 3) == 0) {
				int newMode = LookAheadTest(context.message(), context.currentPos(), encodingMode);
				if (newMode != encodingMode) {
					context.setNewEncoding(newMode);
					break;
				}
			}
		}
		return HandleEOD(context, buffer);
	}

	static void EncodeC40(EncoderContext& context)
	{
		EncodeC40(context, EncodeChar, C40_ENCODATION);
	}

} // C40Encoder

namespace DMTextEncoder {

	static int EncodeChar(int c, std::string& sb)
	{
		if (c == ' ') {
			sb.push_back('\3');
			return 1;
		}
		if (c >= '0' && c <= '9') {
			sb.push_back((char)(c - 48 + 4));
			return 1;
		}
		if (c >= 'a' && c <= 'z') {
			sb.push_back((char)(c - 97 + 14));
			return 1;
		}
		if (c >= '\0' && c <= '\x1f') {
			sb.push_back('\0'); //Shift 1 Set
			sb.push_back(c);
			return 2;
		}
		if (c >= '!' && c <= '/') {
			sb.push_back('\1'); //Shift 2 Set
			sb.push_back((char)(c - 33));
			return 2;
		}
		if (c >= ':' && c <= '@') {
			sb.push_back('\1'); //Shift 2 Set
			sb.push_back((char)(c - 58 + 15));
			return 2;
		}
		if (c >= '[' && c <= '_') {
			sb.push_back('\1'); //Shift 2 Set
			sb.push_back((char)(c - 91 + 22));
			return 2;
		}
		if (c == '\x60') {
			sb.push_back('\2'); //Shift 3 Set
			sb.push_back((char)(c - 96));
			return 2;
		}
		if (c >= 'A' && c <= 'Z') {
			sb.push_back('\2'); //Shift 3 Set
			sb.push_back((char)(c - 65 + 1));
			return 2;
		}
		if (c >= '{' && c <= '\x7f') {
			sb.push_back('\2'); //Shift 3 Set
			sb.push_back((char)(c - 123 + 27));
			return 2;
		}
		if (c >= '\x80') {
			sb.append("\1\x1e"); //Shift 2, Upper Shift
			int len = 2;
			len += EncodeChar((char)(c - 128), sb);
			return len;
		}
		throw std::invalid_argument("Illegal character: " + ToHexString(c));
	}

	static void EncodeText(EncoderContext& context)
	{
		C40Encoder::EncodeC40(context, EncodeChar, C40_ENCODATION);
	}

} // DMTextEncoder

namespace X12Encoder {

	static int EncodeChar(int c, std::string& sb)
	{
		if (c == '\r') {
			sb.push_back('\0');
		}
		else if (c == '*') {
			sb.push_back('\1');
		}
		else if (c == '>') {
			sb.push_back('\2');
		}
		else if (c == ' ') {
			sb.push_back('\3');
		}
		else if (c >= '0' && c <= '9') {
			sb.push_back((char)(c - 48 + 4));
		}
		else if (c >= 'A' && c <= 'Z') {
			sb.push_back((char)(c - 65 + 14));
		}
		else {
			throw std::invalid_argument("Illegal character: " + ToHexString(c));
		}
		return 1;
	}

	static void HandleEOD(EncoderContext context, std::string& buffer)
	{
		int codewordCount = context.codewordCount();
		auto symbolInfo = context.updateSymbolInfo(codewordCount);
		int available = symbolInfo->dataCapacity() - codewordCount;
		context.setCurrentPos(context.currentPos() - static_cast<int>(buffer.length()));
		if (context.remainingCharacters() > 1 || available > 1 || context.remainingCharacters() != available) {
			context.addCodeword(X12_UNLATCH);
		}
		if (context.newEncoding() < 0) {
			context.setNewEncoding(ASCII_ENCODATION);
		}
	}

	static void EncodeX12(EncoderContext& context)
	{
		//step C
		std::string buffer;
		while (context.hasMoreCharacters()) {
			int c = context.currentChar();
			context.setCurrentPos(context.currentPos() + 1);
			EncodeChar(c, buffer);
			size_t count = buffer.length();
			if ((count % 3) == 0) {
				C40Encoder::WriteNextTriplet(context, buffer);

				int newMode = LookAheadTest(context.message(), context.currentPos(), X12_ENCODATION);
				if (newMode != X12_ENCODATION) {
					context.setNewEncoding(newMode);
					break;
				}
			}
		}
		HandleEOD(context, buffer);
	}
	
} // X12Encoder

namespace EdifactEncoder {

	static void EncodeChar(int c, std::string& sb)
	{
		if (c >= ' ' && c <= '?') {
			sb.push_back(c);
		}
		else if (c >= '@' && c <= '^') {
			sb.push_back((char)(c - 64));
		}
		else {
			throw std::invalid_argument("Illegal character: " + ToHexString(c));
		}
	}

	static std::vector<int> EncodeToCodewords(const std::string& sb, int startPos)
	{
		int len = static_cast<int>(sb.length()) - startPos;
		if (len == 0) {
			throw std::invalid_argument("buffer must not be empty");
		}
		int c1 = sb.at(startPos);
		int c2 = len >= 2 ? sb.at(startPos + 1) : 0;
		int c3 = len >= 3 ? sb.at(startPos + 2) : 0;
		int c4 = len >= 4 ? sb.at(startPos + 3) : 0;

		int v = (c1 << 18) + (c2 << 12) + (c3 << 6) + c4;
		int cw1 = (v >> 16) & 255;
		int cw2 = (v >> 8) & 255;
		int cw3 = v & 255;
		std::vector<int> res;
		res.reserve(3);
		res.push_back(cw1);
		if (len >= 2) {
			res.push_back(cw2);
		}
		if (len >= 3) {
			res.push_back(cw3);
		}
		return res;
	}

	/**
	* Handle "end of data" situations
	*
	* @param context the encoder context
	* @param buffer  the buffer with the remaining encoded characters
	*/
	static void HandleEOD(EncoderContext& context, std::string& buffer)
	{
		try {
			size_t count = buffer.length();
			if (count == 0) {
				return; //Already finished
			}
			if (count == 1) {
				//Only an unlatch at the end
				int codewordCount = context.codewordCount();
				auto symbolInfo = context.updateSymbolInfo(codewordCount);
				int available = symbolInfo->dataCapacity() - codewordCount;
				int remaining = context.remainingCharacters();
				if (remaining == 0 && available <= 2) {
					return; //No unlatch
				}
			}

			if (count > 4) {
				throw std::invalid_argument("Count must not exceed 4");
			}
			int restChars = static_cast<int>(count - 1);
			auto encoded = EncodeToCodewords(buffer, 0);
			bool endOfSymbolReached = !context.hasMoreCharacters();
			bool restInAscii = endOfSymbolReached && restChars <= 2;

			if (restChars <= 2) {
				int codewordCount = context.codewordCount();
				auto symbolInfo = context.updateSymbolInfo(codewordCount + restChars);
				int available = symbolInfo->dataCapacity() - codewordCount;
				if (available >= 3) {
					restInAscii = false;
					context.updateSymbolInfo(codewordCount + static_cast<int>(encoded.size()));
					//available = context.symbolInfo.dataCapacity - context.getCodewordCount();
				}
			}

			if (restInAscii) {
				context.resetSymbolInfo();
				context.setCurrentPos(context.currentPos() - restChars);
			}
			else {
				for (int cw : encoded) {
					context.addCodeword(cw);
				}
			}
		}
		catch (...) {
			context.setNewEncoding(ASCII_ENCODATION);
			throw;
		}
		context.setNewEncoding(ASCII_ENCODATION);
	}

	static void EncodeEdifact(EncoderContext& context)
	{
		//step F
		std::string buffer;
		while (context.hasMoreCharacters()) {
			int c = context.currentChar();
			EncodeChar(c, buffer);
			context.setCurrentPos(context.currentPos() + 1);

			if (buffer.length() >= 4) {
				auto codewords = EncodeToCodewords(buffer, 0);
				for (int cw : codewords) {
					context.addCodeword(cw);
				}
				buffer.erase(0, 4);

				int newMode = LookAheadTest(context.message(), context.currentPos(), EDIFACT_ENCODATION);
				if (newMode != EDIFACT_ENCODATION) {
					context.setNewEncoding(ASCII_ENCODATION);
					break;
				}
			}
		}
		buffer.push_back(31); //Unlatch
		HandleEOD(context, buffer);
	}

} // EdifactEncoder

namespace Base256Encoder {

	static int Randomize255State(int ch, int codewordPosition)
	{
		int pseudoRandom = ((149 * codewordPosition) % 255) + 1;
		int tempVariable = ch + pseudoRandom;
		if (tempVariable <= 255) {
			return tempVariable;
		}
		else {
			return tempVariable - 256;
		}
	}

	static void EncodeBase256(EncoderContext& context)
	{
		std::string buffer;
		buffer.push_back('\0'); //Initialize length field
		while (context.hasMoreCharacters()) {
			int c = context.currentChar();
			buffer.push_back(c);

			context.setCurrentPos(context.currentPos() + 1);

			int newMode = LookAheadTest(context.message(), context.currentPos(), BASE256_ENCODATION);
			if (newMode != BASE256_ENCODATION) {
				context.setNewEncoding(newMode);
				break;
			}
		}
		int dataCount = static_cast<int>(buffer.length()) - 1;
		int lengthFieldSize = 1;
		int currentSize = context.codewordCount() + dataCount + lengthFieldSize;
		auto symbolInfo = context.updateSymbolInfo(currentSize);
		bool mustPad = (symbolInfo->dataCapacity() - currentSize) > 0;
		if (context.hasMoreCharacters() || mustPad) {
			if (dataCount <= 249) {
				buffer.at(0) = (char)dataCount;
			}
			else if (dataCount > 249 && dataCount <= 1555) {
				buffer.at(0) = (char)((dataCount / 250) + 249);
				buffer.insert(1, 1, (char)(dataCount % 250));
			}
			else {
				throw std::invalid_argument("Message length not in valid ranges: " + std::to_string(dataCount));
			}
		}
		for (char c : buffer) {
			context.addCodeword(Randomize255State(c, context.codewordCount() + 1));
		}
	}

} // Base256Encoder

static bool StartsWith(const std::wstring& s, const std::wstring& ss)
{
	return s.length() > ss.length() && s.compare(0, ss.length(), ss) == 0;
}

static bool EndsWith(const std::wstring& s, const std::wstring& ss)
{
	return s.length() > ss.length() && s.compare(s.length() - ss.length(), ss.length(), ss) == 0;
}

/**
* Performs message encoding of a DataMatrix message using the algorithm described in annex P
* of ISO/IEC 16022:2000(E).
*
* @param msg     the message
* @param shape   requested shape. May be {@code SymbolShapeHint.FORCE_NONE},
*                {@code SymbolShapeHint.FORCE_SQUARE} or {@code SymbolShapeHint.FORCE_RECTANGLE}.
* @param minSize the minimum symbol size constraint or null for no constraint
* @param maxSize the maximum symbol size constraint or null for no constraint
* @return the encoded message (the char values range from 0 to 255)
*/
void
HighLevelEncoder::Encode(const std::wstring& msg, SymbolShape shape, int minWdith, int minHeight, int maxWidth, int maxHeight, std::vector<int>& output)
{
	//the codewords 0..255 are encoded as Unicode characters
	//Encoder[] encoders = {
	//	new ASCIIEncoder(), new C40Encoder(), new TextEncoder(),
	//	new X12Encoder(), new EdifactEncoder(),  new Base256Encoder()
	//};

	std::string bytes;
	TextEncoder::GetBytes(msg, CharacterSet::ISO8859_1, bytes);
	EncoderContext context(bytes);
	context.setSymbolShape(shape);
	context.setSizeConstraints(minWdith, minHeight, maxWidth, maxHeight);

	if (StartsWith(msg, MACRO_05_HEADER) && EndsWith(msg, MACRO_TRAILER)) {
		context.addCodeword(MACRO_05);
		context.setSkipAtEnd(2);
		context.setCurrentPos(static_cast<int>(MACRO_05_HEADER.length()));
	}
	else if (StartsWith(msg, MACRO_06_HEADER) && EndsWith(msg, MACRO_TRAILER)) {
		context.addCodeword(MACRO_06);
		context.setSkipAtEnd(2);
		context.setCurrentPos(static_cast<int>(MACRO_06_HEADER.length()));
	}

	int encodingMode = ASCII_ENCODATION; //Default mode
	while (context.hasMoreCharacters()) {
		switch (encodingMode) {
			case ASCII_ENCODATION:   ASCIIEncoder::EncodeASCII(context);     break;
			case C40_ENCODATION:     C40Encoder::EncodeC40(context);         break;
			case TEXT_ENCODATION:    DMTextEncoder::EncodeText(context);     break;
			case X12_ENCODATION:     X12Encoder::EncodeX12(context);         break;
			case EDIFACT_ENCODATION: EdifactEncoder::EncodeEdifact(context); break;
			case BASE256_ENCODATION: Base256Encoder::EncodeBase256(context); break;
		}
		if (context.newEncoding() >= 0) {
			encodingMode = context.newEncoding();
			context.clearNewEncoding();
		}
	}
	int len = context.codewordCount();
	auto symbolInfo = context.updateSymbolInfo(len);
	int capacity = symbolInfo->dataCapacity();
	if (len < capacity) {
		if (encodingMode != ASCII_ENCODATION && encodingMode != BASE256_ENCODATION) {
			context.addCodeword('\xfe'); //Unlatch (254)
		}
	}
	//Padding
	if (context.codewordCount() < capacity) {
		context.addCodeword(PAD);
	}
	while (context.codewordCount() < capacity) {
		context.addCodeword(Randomize253State(PAD, context.codewordCount() + 1));
	}

	output = context.codewords();
}

} // DataMatrix
} // ZXing