#include <cstdint>
#include <cstdlib>
#include <cstring> // memcpy
#include <ctime> // strftime
#include <cmath> // isfinite
#include <thread>
#include <mutex>
#include <condition_variable>
#include "napi.h"
#include "../deps/double_conversion/double-to-string.h"
#include "cpu-detection.h"

#ifdef _MSC_VER
# include <intrin.h>
#elif defined(__GNUC__)
# include <x86intrin.h>
#endif

using namespace double_conversion;

using std::size_t;
using std::uint8_t;
using std::int32_t;
using std::uint32_t;
using std::int64_t;

constexpr uint8_t BSON_DATA_NUMBER = 1;
constexpr uint8_t BSON_DATA_STRING = 2;
constexpr uint8_t BSON_DATA_OBJECT = 3;
constexpr uint8_t BSON_DATA_ARRAY = 4;
constexpr uint8_t BSON_DATA_BINARY = 5;
constexpr uint8_t BSON_DATA_UNDEFINED = 6;
constexpr uint8_t BSON_DATA_OID = 7;
constexpr uint8_t BSON_DATA_BOOLEAN = 8;
constexpr uint8_t BSON_DATA_DATE = 9;
constexpr uint8_t BSON_DATA_NULL = 10;
constexpr uint8_t BSON_DATA_REGEXP = 11;
constexpr uint8_t BSON_DATA_DBPOINTER = 12;
constexpr uint8_t BSON_DATA_CODE = 13;
constexpr uint8_t BSON_DATA_SYMBOL = 14;
constexpr uint8_t BSON_DATA_CODE_W_SCOPE = 15;
constexpr uint8_t BSON_DATA_INT = 16;
constexpr uint8_t BSON_DATA_TIMESTAMP = 17;
constexpr uint8_t BSON_DATA_LONG = 18;
constexpr uint8_t BSON_DATA_DECIMAL128 = 19;
constexpr uint8_t BSON_DATA_MIN_KEY = 0xff;
constexpr uint8_t BSON_DATA_MAX_KEY = 0x7f;

// Adaptetd from https://github.com/fmtlib/fmt/blob/master/include/fmt/format.h#L2818,
// MIT license
constexpr const char digits[] =
	"0001020304050607080910111213141516171819"
	"2021222324252627282930313233343536373839"
	"4041424344454647484950515253545556575859"
	"6061626364656667686970717273747576777879"
	"8081828384858687888990919293949596979899";

template<typename T> constexpr size_t INT_BUF_DIGS = 0;
template<> constexpr size_t INT_BUF_DIGS<int32_t> = 11;
template<> constexpr size_t INT_BUF_DIGS<int64_t> = 20;

template<typename T>
size_t fast_itoa(uint8_t* &p, T val) {
	p += INT_BUF_DIGS<T>;
	size_t n = 0;

	const bool isNegative = val < 0;
	if (isNegative)
		val = 0 - val;

	while (val >= 100) {
		size_t index = static_cast<size_t>((val % 100) * 2);
		val /= 100;
		*--p = digits[index + 1];
		*--p = digits[index];
		n += 2;
	}

	if (val < 10) {
		*--p = static_cast<uint8_t>('0' + val);
		n++;
	} else {
		size_t index = static_cast<size_t>(val * 2);
		*--p = digits[index + 1];
		*--p = digits[index];
		n += 2;
	}

	if (isNegative) {
		*--p = '-';
		n++;
	}

	return n;
}

// Returns the char to use to escape c if it requires escaping, else returns 0.
inline static uint8_t getEscape(uint8_t c) {
	switch (c) {
	case 0x08: return 'b';
	case 0x09: return 't';
	case 0x0a: return 'n';
	case 0x0c: return 'f';
	case 0x0d: return 'r';
	case 0x22: return c; // "
	case 0x5c: return c; /* \ */
	default: return 0;
	}
}

constexpr const char HEX_DIGITS[] = "0123456789abcdef";

inline static constexpr uint8_t hexNib(uint8_t nib) {
	// These appear equally fast.
	return HEX_DIGITS[nib];
	// return nib + (nib < 10 ? 48 : 87);
}

// Returns the number of base-10 digits in a null-terminated string
// representation of v. Assumes relatively small arrays. See
// https://stackoverflow.com/a/1489873/1218408 for a version that might be
// better for medium and large arrays.
inline static constexpr size_t nDigits(int32_t v) {
	if (v < 10) return 2;
	if (v < 100) return 3;
	if (v < 1'000) return 4;
	if (v < 10'000) return 5;
	if (v < 100'000) return 6;
	if (v < 1'000'000) return 7;
	if (v < 10'000'000) return 8;
	if (v < 100'000'000) return 9;
	if (v < 1'000'000'000) return 10;
	return 11;
}

// Except for MSVC and maybe some C++14-compliant compilers, class member fns
// can't be specialized, which I want to do for ISA specialization. This is sort
// of a hack that uses overload resolution instead, based on:
// https://stackoverflow.com/a/38283990/1218408
template<int isa> struct Enabler : Enabler<isa - 1> {};
template<> struct Enabler<0> {};

#define ENSURE_SPACE_OR_RETURN(n) if (ensureSpace(n)) return true

class Transcoder {
public:
	enum Mode {
		/* Reallocate the output buffer as necessary to fit entire contents. */
		REALLOC,
		/* Pause when output buffer is full. Adjust `out`, `outIdx` and
		 * `outLen`, then call `resume` when ready to continue.
		 */
		PAUSE
	};

	uint8_t* out = nullptr;
	size_t outIdx = 0;
	size_t outLen = 0;
	char const* err = nullptr;

	/// <summary>
	/// Transcodes the BSON document to JSON. Call once per lifetime of the
	/// Transcoder instance.
	/// </summary>
	/// <param name="in_">BSON document</param>
	/// <param name="inLen_">Length of input</param>
	/// <param name="isArray">Whether or not the document is an array.</param>
	/// <param name="chunkSize">
	/// Initial size of the output buffer. Setting to 0 currently uses 2.5x
	/// the inLen.
	/// </param>
	/// <param name="mode_">What to do when the output buffer is full.</param>
	/// <param name="fixedOut">With Mode::PAUSE, output buffer to reuse.</param>
	template <ISA isa>
	bool transcode(const uint8_t* in_, size_t inLen_, bool isArray = true,
			size_t chunkSize = 0, Mode mode_ = Mode::REALLOC, uint8_t* fixedOut = nullptr) {

		in = in_;
		inLen = inLen_;
		inIdx = 0;
		mode = mode_;

		if (chunkSize == 0 && fixedOut == nullptr) {
			// Estimate outLen at 2.5x inLen. Expansion rates for values:
			// ObjectId: 12B -> 24B plus 2 for quotes
			// String: 5 for header + 1 per char -> 1 or 2 per char + 2 for quotes
			// Int: 1+4 -> up to 11
			// Long: 1+8 -> up to 20
			// Number: 1+8 -> up to ???
			// Date: 1+8 -> 24 plus 2 for quotes
			// Boolean: 1+1 -> 4 or 5
			// Null: 1+0 -> 4
			// The maximum expansion ratio is 1:5 (for null), but averages ~2.3x
			// for mixed data or ~1x for string-heavy data.
			chunkSize = (inLen * 10) >> 2;
		}

		if (fixedOut == nullptr) {
			resize(chunkSize);
		} else {
			out = fixedOut;
			outLen = chunkSize;
		}

		if (mode == Mode::PAUSE) {
			std::unique_lock<std::mutex> lk(m);
			outIdx = outLen + 1;
			cv.wait(lk, [&] { return outIdx == 0; });
		}

		if (transcodeObject<isa>(isArray))
			return true;

		if (mode == Mode::PAUSE)
			cv.notify_one();
		
		return false;
	}

	void setOutIdx(size_t to) {
		std::unique_lock<std::mutex> lk(m);
		outIdx = to;
	}

	void resume() {
		cv.notify_one();
		std::unique_lock<std::mutex> lk(m);
		cv.wait(lk, [&] { return outIdx > 0 || isDone(); });
	}

	bool isDone() {
		return inIdx == inLen;
	}

	void destroy() {
		std::free(out);
		out = nullptr;
		outIdx = 0;
	}

private:
	const uint8_t* in = nullptr;
	size_t inIdx = 0;
	size_t inLen = 0;
	Mode mode;
	std::mutex m;
	std::condition_variable cv;

	template<typename T>
	inline T readLE() {
		T v;
		std::memcpy(&v, in + inIdx, sizeof(T));
		inIdx += sizeof(T);
		return v;
	}

	bool resize(size_t to) {
		// printf("resizing from %d to %d\n", outLen, to);
		uint8_t* oldOut = out;
		out = static_cast<uint8_t*>(std::realloc(out, to));
		if (out == nullptr) {
			std::free(oldOut);
			err = "Allocation failure";
			return true;
		}
		// printf("allocated %p\n", out);
		outLen = to;
		return false;
	}

	[[nodiscard]] inline bool ensureSpace(size_t n) {
		if (outIdx + n < outLen) [[likely]] {
			return false;
		}

		if (mode == Mode::REALLOC) {
			if (resize((outLen * 3) >> 1))
				return true;
		} else {
			cv.notify_one();
			std::unique_lock<std::mutex> lk(m);
			cv.wait(lk, [&] { return outIdx == 0; });
		}
		return false;
	}

	// Writes the `\ u 0 0 ch cl` sequence
	inline void writeControlChar(uint8_t c) {
		out[outIdx++] = '\\';
		out[outIdx++] = 'u';
		out[outIdx++] = '0';
		out[outIdx++] = '0';
		out[outIdx++] = (c & 0xf0) ? '1' : '0';
		out[outIdx++] = hexNib(c & 0xf);
	}

	// Writes n characters from in to out, escaping per ECMA-262 sec 24.5.2.2.
	bool writeEscapedChars(size_t n, Enabler<ISA::BASELINE>) {
		const size_t end = inIdx + n;
		// TODO the inner ensureSpace can be skipped when ensureSpace(n * 6) is
		// true (worst-case expansion is 6x).
		ENSURE_SPACE_OR_RETURN(n);
		while (inIdx < end) {
			uint8_t xc;
			const uint8_t c = in[inIdx++];
			if (c >= 0x20 && c != 0x22 && c != 0x5c) {
				out[outIdx++] = c;
			} else if ((xc = getEscape(c))) { // single char escape
				ENSURE_SPACE_OR_RETURN(end - inIdx + 1);
				out[outIdx++] = '\\';
				out[outIdx++] = xc;
			} else { // c < 0x20, control
				ENSURE_SPACE_OR_RETURN(end - inIdx + 5);
				writeControlChar(c);
			}
		}
		return false;
	}

	bool writeEscapedChars(size_t n, Enabler<ISA::SSE42>) {
		const size_t end = inIdx + n;
		ENSURE_SPACE_OR_RETURN(n);

		const __m128i escapes = _mm_set_epi8(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x5c,0x5c, 0x22,0x22, 0x1f,0);

		while (inIdx < end) {
			const int clampedN = n > 16 ? 16 : n;
			__m128i chars = _mm_loadu_si128(reinterpret_cast<__m128i const*>(&in[inIdx])); // TODO this can overrun
			int esRIdx = _mm_cmpestri(escapes, 6, chars, n,
				_SIDD_UBYTE_OPS | _SIDD_CMP_RANGES | _SIDD_POSITIVE_POLARITY | _SIDD_LEAST_SIGNIFICANT);

			if (esRIdx == 16) {
				// No chars need escaping.
				esRIdx = clampedN;
			}

			_mm_storeu_si128(reinterpret_cast<__m128i*>(&out[outIdx]), chars); // TODO this overruns (okay except at end)
			//memcpy(&out[outIdx], &in[inIdx], esRIdx);
			n -= esRIdx;
			outIdx += esRIdx;
			inIdx += esRIdx;

			if (esRIdx < clampedN) {
				uint8_t xc;
				uint8_t c = in[inIdx++];
				n--;
				if ((xc = getEscape(c))) { // single char escape
					ENSURE_SPACE_OR_RETURN(end - inIdx + 1);
					out[outIdx++] = '\\';
					out[outIdx++] = xc;
				} else { // c < 0x20, control
					ENSURE_SPACE_OR_RETURN(end - inIdx + 5);
					writeControlChar(c);
				}
			}
		}
		return false;
	}

	bool writeEscapedChars(size_t n, Enabler<ISA::SSE2>) {
		const size_t end = inIdx + n;
		ENSURE_SPACE_OR_RETURN(n);

		// escape if (x < 0x20 || x == 0x22 || x == 0x5c)

		// xor 0x80 to get unsigned comparison. https://stackoverflow.com/q/32945410/1218408
		__m128i esch20 = _mm_set1_epi8(0x20 ^ 0x80);
		__m128i esch22 = _mm_set1_epi8(0x22);
		__m128i esch5c = _mm_set1_epi8(0x5c);

		while (inIdx < end) {
			const size_t clampedN = n > 16 ? 16 : n;
			__m128i chars = _mm_loadu_si128(reinterpret_cast<__m128i const*>(&in[inIdx])); // TODO this can overrun (okay except at end)

			// see above (unsigned comparison)
			__m128i iseq = _mm_cmpgt_epi8(esch20, _mm_xor_si128(chars, _mm_set1_epi8(0x80)));
			iseq = _mm_or_si128(iseq, _mm_cmpeq_epi8(chars, esch22));
			iseq = _mm_or_si128(iseq, _mm_cmpeq_epi8(chars, esch5c));

			uint32_t mask = _mm_movemask_epi8(iseq);
			uint32_t esRIdx = _tzcnt_u32(mask);

			if (esRIdx > clampedN) {
				// No chars need escaping.
				esRIdx = clampedN;
			}

			_mm_storeu_si128(reinterpret_cast<__m128i*>(&out[outIdx]), chars); // TODO this overruns (okay except at end)
			//memcpy(&out[outIdx], &in[inIdx], esRIdx);
			n -= esRIdx;
			outIdx += esRIdx;
			inIdx += esRIdx;

			if (esRIdx < clampedN) {
				uint8_t xc;
				uint8_t c = in[inIdx++];
				n--;
				if ((xc = getEscape(c))) { // single char escape
					ENSURE_SPACE_OR_RETURN(end - inIdx + 1);
					out[outIdx++] = '\\';
					out[outIdx++] = xc;
				} else { // c < 0x20, control
					ENSURE_SPACE_OR_RETURN(end - inIdx + 5);
					writeControlChar(c);
				}
			}
		}
		return false;
	}

	bool writeEscapedChars(size_t n, Enabler<ISA::AVX2>) {
		const size_t end = inIdx + n;
		ENSURE_SPACE_OR_RETURN(n);

		// escape if (x < 0x20 || x == 0x22 || x == 0x5c)

		// xor 0x80 to get unsigned comparison. https://stackoverflow.com/q/32945410/1218408
		__m256i esch20 = _mm256_set1_epi8(0x20 ^ 0x80);
		__m256i esch22 = _mm256_set1_epi8(0x22);
		__m256i esch5c = _mm256_set1_epi8(0x5c);

		while (inIdx < end) {
			const size_t clampedN = n > 32 ? 32 : n;
			__m256i chars = _mm256_loadu_si256(reinterpret_cast<__m256i const*>(&in[inIdx])); // TODO this can overrun (okay except at end)

			// see above (unsigned comparison)
			__m256i iseq = _mm256_cmpgt_epi8(esch20, _mm256_xor_si256(chars, _mm256_set1_epi8(0x80)));
			iseq = _mm256_or_si256(iseq, _mm256_cmpeq_epi8(chars, esch22));
			iseq = _mm256_or_si256(iseq, _mm256_cmpeq_epi8(chars, esch5c));

			uint32_t mask = _mm256_movemask_epi8(iseq);
			uint32_t esRIdx = _tzcnt_u32(mask);

			if (esRIdx > clampedN) {
				// No chars need escaping.
				esRIdx = clampedN;
			}

			_mm256_storeu_si256(reinterpret_cast<__m256i*>(&out[outIdx]), chars); // TODO this overruns (okay except at end)
			//memcpy(&out[outIdx], &in[inIdx], esRIdx);
			n -= esRIdx;
			outIdx += esRIdx;
			inIdx += esRIdx;

			if (esRIdx < clampedN) {
				uint8_t xc;
				uint8_t c = in[inIdx++];
				n--;
				if ((xc = getEscape(c))) { // single char escape
					ENSURE_SPACE_OR_RETURN(end - inIdx + 1);
					out[outIdx++] = '\\';
					out[outIdx++] = xc;
				} else { // c < 0x20, control
					ENSURE_SPACE_OR_RETURN(end - inIdx + 5);
					writeControlChar(c);
				}
			}
		}
		return false;
	}

	// Writes the null-terminated string from in to out, escaping per JSON spec.
	bool writeEscapedChars(Enabler<ISA::BASELINE>) {
		// TODO the inner ensureSpace can be skipped when ensureSpace(n * 6) is
		// true (worst-case expansion is 6x).
		uint8_t c;
		while ((c = in[inIdx++])) {
			uint8_t xc;
			if (c >= 0x20 && c != 0x22 && c != 0x5c) {
				ENSURE_SPACE_OR_RETURN(1);
				out[outIdx++] = c;
			} else if ((xc = getEscape(c))) { // single char escape
				ENSURE_SPACE_OR_RETURN(2);
				out[outIdx++] = '\\';
				out[outIdx++] = xc;
			} else { // c < 0x20, control
				ENSURE_SPACE_OR_RETURN(6);
				writeControlChar(c);
			}
		}
		inIdx--;
		return false;
	}

	// TODO SSE2

	bool writeEscapedChars(Enabler<ISA::SSE42>) {
		// escape if (x < 0x20 || x == 0x22 || x == 0x5c)
		const __m128i escapes = _mm_set_epi8(0,0, 0,0, 0,0, 0,0, 0,0, 0xff,0x5d, 0x5b,0x23, 0x21,0x20);

		while (inIdx < inLen) {
			__m128i chars = _mm_loadu_si128(reinterpret_cast<__m128i const*>(&in[inIdx])); // TODO this can overrun
			int esRIdx = _mm_cmpistri(escapes, chars,
				_SIDD_UBYTE_OPS | _SIDD_CMP_RANGES | _SIDD_NEGATIVE_POLARITY | _SIDD_LEAST_SIGNIFICANT);

			ENSURE_SPACE_OR_RETURN(esRIdx);
			_mm_storeu_si128(reinterpret_cast<__m128i*>(&out[outIdx]), chars); // TODO this overruns (okay except at end)
			//memcpy(&out[outIdx], &in[inIdx], esRIdx);
			outIdx += esRIdx;
			inIdx += esRIdx;

			if (esRIdx < 16) {
				if (in[inIdx] == 0)
					return false;

				uint8_t xc;
				uint8_t c = in[inIdx++];
				if ((xc = getEscape(c))) { // single char escape
					ENSURE_SPACE_OR_RETURN(2);
					out[outIdx++] = '\\';
					out[outIdx++] = xc;
				} else { // c < 0x20, control
					ENSURE_SPACE_OR_RETURN(6);
					writeControlChar(c);
				}
			}
		}
		return false;
	}

	bool writeEscapedChars(Enabler<ISA::AVX2>) {
		// escape if (x < 0x20 || x == 0x22 || x == 0x5c)

		// TODO see explicit length version's handling of unsigned comparison.
		__m256i esch20 = _mm256_set1_epi8(0x20);
		__m256i esch22 = _mm256_set1_epi8(0x22);
		__m256i esch5c = _mm256_set1_epi8(0x5c);

		while (inIdx < inLen) {
			__m256i chars = _mm256_loadu_si256(reinterpret_cast<__m256i const*>(&in[inIdx])); // TODO this can overrun

			__m256i iseq = _mm256_cmpgt_epi8(esch20, chars);
			iseq = _mm256_or_si256(iseq, _mm256_cmpeq_epi8(chars, esch22));
			iseq = _mm256_or_si256(iseq, _mm256_cmpeq_epi8(chars, esch5c));

			uint32_t mask = _mm256_movemask_epi8(iseq);
			uint32_t esRIdx = _tzcnt_u32(mask); // position of 0 *or* a char that needs to be escaped

			ENSURE_SPACE_OR_RETURN(esRIdx);
			_mm256_storeu_si256(reinterpret_cast<__m256i*>(&out[outIdx]), chars); // TODO this overruns (okay except at end)
			//memcpy(&out[outIdx], &in[inIdx], esRIdx);
			outIdx += esRIdx;
			inIdx += esRIdx;

			if (esRIdx < 32) {
				if (in[inIdx] == 0)
					return false;
				
				uint8_t xc;
				uint8_t c = in[inIdx++];
				if ((xc = getEscape(c))) { // single char escape
					ENSURE_SPACE_OR_RETURN(2);
					out[outIdx++] = '\\';
					out[outIdx++] = xc;
				} else { // c < 0x20, control
					ENSURE_SPACE_OR_RETURN(6);
					writeControlChar(c);
				}
			}
		}
		return false;
	}

	inline void transcodeObjectId(Enabler<ISA::BASELINE>) {
		out[outIdx++] = '"';
		const size_t end = inIdx + 12;
		while (inIdx < end) {
			uint8_t byte = in[inIdx++];
			out[outIdx++] = hexNib(byte >> 4);
			out[outIdx++] = hexNib(byte & 0xf);
		}
		out[outIdx++] = '"';
	}

	// TODO SSE2: arithmetic (nib + (nib < 10 ? 48 : 87))
	// TODO SSSE3: 128-bit pshufb

	inline void transcodeObjectId(Enabler<ISA::AVX2>) {
		// TODO mask load is 4,0.5, load is 3,0.5. Overrun the load where possible:
		// if (not at end of buffer) {
		//	 __m128i a = _mm_loadu_si128(reinterpret_cast<__m128i const*>(in + inIdx));
		// } else {
		__m128i loadmask = _mm_set_epi32(0, 0x80000000, 0x80000000, 0x80000000); // 12B = 3x4B
		__m128i a = _mm_maskload_epi32(reinterpret_cast<int const*>(in + inIdx), loadmask);
		// }
		inIdx += 12;

		// Technique from https://github.com/zbjornson/fast-hex
		const __m256i HEX_LUTR = _mm256_setr_epi8(
			'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
			'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f');
		const __m256i ROT2 = _mm256_setr_epi8(
			-1, 0, -1, 2, -1, 4, -1, 6, -1, 8, -1, 10, -1, 12, -1, 14,
			-1, 0, -1, 2, -1, 4, -1, 6, -1, 8, -1, 10, -1, 12, -1, 14
		);
		// Bytes to nibbles (a -> [a >> 4, a & 0b1111]):
		__m256i doubled = _mm256_cvtepu8_epi16(a);
		__m256i hi = _mm256_srli_epi16(doubled, 4);
		__m256i lo = _mm256_shuffle_epi8(doubled, ROT2);
		__m256i bytes = _mm256_or_si256(hi, lo); // TODO AVX512: mask_shuffle to avoid the OR
		bytes = _mm256_and_si256(bytes, _mm256_set1_epi8(0b1111));
		// Encode hex
		__m256i b = _mm256_shuffle_epi8(HEX_LUTR, bytes);

		out[outIdx++] = '"';
		// TODO mask store is 14,1, store is 3,1. Overrun the store where possible:
		// if (not at end of buffer) {
		//   _mm256_storeu_si256(reinterpret_cast<__m256i*>(out + outIdx), b);
		// } else {
		
		// Split store:
		// _mm_storeu_si128(reinterpret_cast<__m128i*>(out + outIdx), _mm256_castsi256_si128(b)); // 16B
		// *reinterpret_cast<uint64_t*>(out + outIdx + 16) = _mm256_extract_epi64(b, 3);

		// Masked store:
		__m256i storemask = _mm256_set_epi32(0, 0, 0x80000000, 0x80000000,
			0x80000000, 0x80000000, 0x80000000, 0x80000000); // 24B = 6x4B
		_mm256_maskstore_epi32(reinterpret_cast<int*>(out + outIdx), storemask, b);
		// }
		outIdx += 24;
		out[outIdx++] = '"';
	}

	template<ISA isa>
	bool transcodeObject(bool isArray) {
		const int32_t size = readLE<int32_t>();
		if (size < 5) {
			err = "BSON size must be >=5";
			return true;
		}
		if (size > inLen) {// + inIdx?
			err = "BSON size exceeds input length.";
			return true;
		}

		int32_t arrIdx = 0;

		ENSURE_SPACE_OR_RETURN(1);
		out[outIdx++] = isArray ? '[' : '{';

		while (true) {
			const uint8_t elementType = in[inIdx++];
			if (elementType == 0) break;

			if (arrIdx) {
				ENSURE_SPACE_OR_RETURN(1);
				out[outIdx++] = ',';
			}

			// Write name
			if (!isArray) {
				ENSURE_SPACE_OR_RETURN(1);
				out[outIdx++] = '"';
				writeEscapedChars(Enabler<isa>{});
				inIdx++; // skip null terminator
				ENSURE_SPACE_OR_RETURN(2);
				out[outIdx++] = '"';
				out[outIdx++] = ':';
			} else {
				inIdx += nDigits(arrIdx);
			}

			switch (elementType) {
			case BSON_DATA_STRING: {
				const int32_t size = readLE<int32_t>();
				if (size <= 0 || size > inLen - inIdx) {
					err = "Bad string length";
					return true;
				}
				ENSURE_SPACE_OR_RETURN(1);
				out[outIdx++] = '"';
				writeEscapedChars(size - 1, Enabler<isa>{});
				inIdx++; // skip null terminator
				ENSURE_SPACE_OR_RETURN(1);
				out[outIdx++] = '"';
				break;
			}
			case BSON_DATA_OID: {
				ENSURE_SPACE_OR_RETURN(26);
				transcodeObjectId(Enabler<isa>{});
				break;
			}
			case BSON_DATA_INT: {
				const int32_t value = readLE<int32_t>();
				uint8_t temp[INT_BUF_DIGS<int32_t>];
				uint8_t* temp_p = temp;
				size_t n = fast_itoa(temp_p, value);
				ENSURE_SPACE_OR_RETURN(n);
				memcpy(out + outIdx, temp_p, n);
				outIdx += n;
				break;
			}
			case BSON_DATA_NUMBER: {
				const double value = readLE<double>();
				if (std::isfinite(value)) {
					constexpr size_t kBufferSize = 128;
					ENSURE_SPACE_OR_RETURN(kBufferSize);
					StringBuilder sb(reinterpret_cast<char*>(out + outIdx), kBufferSize);
					auto& dc = DoubleToStringConverter::EcmaScriptConverter();
					dc.ToShortest(value, &sb);
					outIdx += sb.position();
				} else {
					ENSURE_SPACE_OR_RETURN(4);
					out[outIdx++] = 'n';
					out[outIdx++] = 'u';
					out[outIdx++] = 'l';
					out[outIdx++] = 'l';
				}
				break;
			}
			case BSON_DATA_DATE: {
				ENSURE_SPACE_OR_RETURN(26);
				const int64_t value = readLE<int64_t>(); // BSON encodes UTC ms since Unix epoch
				const time_t seconds = value / 1000;
				const int32_t millis = value % 1000;
				out[outIdx++] = '"';
				size_t n = strftime(reinterpret_cast<char*>(out + outIdx), 24, "%FT%T", gmtime(&seconds));
				outIdx += n;
				out[outIdx++] = '.';
				uint8_t temp[INT_BUF_DIGS<int32_t>];
				uint8_t* temp_p = temp;
				n = fast_itoa(temp_p, millis);
				out[outIdx++] = n < 3 ? '0' : *temp_p++;
				out[outIdx++] = n < 2 ? '0' : *temp_p++;
				out[outIdx++] = n < 1 ? '0' : *temp_p++;
				out[outIdx++] = 'Z';
				out[outIdx++] = '"';
				break;
			}
			case BSON_DATA_BOOLEAN: {
				const uint8_t val = in[inIdx++];
				if (val == 1) {
					ENSURE_SPACE_OR_RETURN(4);
					memcpy(out + outIdx, "true", 4);
					outIdx += 4;
				} else {
					ENSURE_SPACE_OR_RETURN(5);
					memcpy(out + outIdx, "false", 5);
					outIdx += 5;
				}
				break;
			}
			case BSON_DATA_OBJECT: {
				if (transcodeObject<isa>(false))
					return true;
				break;
			}
			case BSON_DATA_ARRAY: {
				if (transcodeObject<isa>(true))
					return true;
				if (in[inIdx - 1] != 0) {
					err = "Invalid array terminator byte";
					return true;
				}
				break;
			}
			case BSON_DATA_NULL: {
				ENSURE_SPACE_OR_RETURN(4);
				memcpy(out + outIdx, "null", 4);
				outIdx += 4;
				break;
			}
			case BSON_DATA_LONG: {
				const int64_t value = readLE<int64_t>();
				uint8_t temp[INT_BUF_DIGS<int64_t>];
				uint8_t* temp_p = temp;
				size_t n = fast_itoa(temp_p, value);
				ENSURE_SPACE_OR_RETURN(n);
				memcpy(out + outIdx, temp_p, n);
				outIdx += n;
				break;
			}
			case BSON_DATA_UNDEFINED:
				// noop
				break;
			case BSON_DATA_DECIMAL128:
			case BSON_DATA_BINARY:
			case BSON_DATA_REGEXP:
			case BSON_DATA_SYMBOL:
			case BSON_DATA_TIMESTAMP:
			case BSON_DATA_MIN_KEY:
			case BSON_DATA_MAX_KEY:
			case BSON_DATA_CODE:
			case BSON_DATA_CODE_W_SCOPE:
			case BSON_DATA_DBPOINTER:
				// incompatible JSON type
				err = "BSON type incompatible with JSON";
				return true;
			default:
				err = "Unknown BSON type";
				return true;
			}

			arrIdx++;
		}

		ENSURE_SPACE_OR_RETURN(1);
		out[outIdx++] = isArray ? ']' : '}';
		return false;
	}
};

Napi::FunctionReference constructor;

template<ISA isa>
class BJTrans : public Napi::ObjectWrap<BJTrans<isa> > {
public:

	static Napi::Object Init(Napi::Env env, Napi::Object exports) {
		Napi::HandleScope scope(env);
		Napi::Function func = DefineClass(env, "BJTrans", {
			InstanceMethod(Napi::Symbol::WellKnown(env, "iterator"), &BJTrans::Iterator)
			});
		
		constructor = Napi::Persistent(func);
		constructor.SuppressDestruct();

		exports.Set("BJTrans", func);
		return exports;
	}

	BJTrans(const Napi::CallbackInfo& info) : Napi::ObjectWrap<BJTrans>(info) {
		Napi::Env env = info.Env();
		Napi::HandleScope scope(env);
		Napi::Uint8Array arr = info[0].As<Napi::Uint8Array>();
		bool isArray = info[1].ToBoolean().Value();
		uint8_t* out = nullptr;

		if (info.Length() >= 2 && info[2].IsObject()) {
			Napi::Object options = info[2].ToObject();

			if (options.Has("chunkSize")) {
				Napi::Value csOpt = options.Get("chunkSize");
				if (csOpt.IsNumber()) {
					chunkSize = csOpt.As<Napi::Number>().Uint32Value();
				} else {
					Napi::TypeError::New(env, "chunkSize must be a number").ThrowAsJavaScriptException();
				}
			}

			if (options.Has("fixedBuffer")) {
				auto val = options.Get("fixedBuffer");
				if (val.IsArrayBuffer()) {
					auto ab = options.Get("fixedBuffer").As<Napi::ArrayBuffer>();
					chunkSize = ab.ByteLength();
					out = static_cast<uint8_t*>(ab.Data());
					fixedBuffer = true;
				} else {
					Napi::TypeError::New(env, "fixedBuffer must be an ArrayBuffer").ThrowAsJavaScriptException();
				}
			}
		}

		worker = std::thread { &Transcoder::transcode<isa>, &trans,
			arr.Data(), arr.ByteLength(), isArray, chunkSize, Transcoder::Mode::PAUSE, out };
	}

private:
	Transcoder trans;
	std::thread worker;
	bool fixedBuffer = false;
	size_t chunkSize = 0;
	bool final = false;

	Napi::Value BJTrans::Iterator(const Napi::CallbackInfo& info) {
		Napi::Env env = info.Env();

		Napi::Object iter = Napi::Object::New(env);

		iter.Set("next", Napi::Function::New(env, [&](const Napi::CallbackInfo& info) -> Napi::Object {
			Napi::Env env = info.Env();

			Napi::Object rv = Napi::Object::New(env);

			if (final) {
				rv.Set("done", true);
				worker.join();
			} else {
				trans.setOutIdx(0);
				trans.resume();

				if (trans.isDone())
					final = true;

				Napi::Buffer<uint8_t> buf;
				if (fixedBuffer) {
					buf = Napi::Buffer<uint8_t>::New(env, trans.out, trans.outIdx, [](Napi::Env, uint8_t* data) {});
				} else {
					buf = Napi::Buffer<uint8_t>::Copy(env, trans.out, trans.outIdx);
				}
				rv.Set("value", buf);
				rv.Set("done", false);
			}

			return rv;
		}, "next"));

		return iter;
	}
};

template<ISA isa>
Napi::Value bsonToJson(const Napi::CallbackInfo& info) {
	Napi::Env env = info.Env();

	Napi::Uint8Array arr = info[0].As<Napi::Uint8Array>();
	bool isArray = info[1].ToBoolean().Value();

	Transcoder trans;
	bool status = trans.transcode<isa>(arr.Data(), arr.ByteLength(), isArray);
	if (status) {
		std::free(trans.out);
		Napi::Error::New(env, trans.err).ThrowAsJavaScriptException();
		return Napi::Value();
	}

	Napi::Buffer<uint8_t> buf = Napi::Buffer<uint8_t>::New(env, trans.out, trans.outIdx, [](Napi::Env, uint8_t* data) {
		// printf("freeing %p\n", data);
		std::free(data);
	});

	return buf;
}

Napi::Object Init(Napi::Env env, Napi::Object exports) {
	Napi::Function fn;
	char* isa;
	if (supports<ISA::AVX2>()) {
		fn = Napi::Function::New(env, bsonToJson<ISA::AVX2>);
		BJTrans<ISA::AVX2>::Init(env, exports);
		isa = "AVX2";
	} else if (supports<ISA::SSE42>()) {
		fn = Napi::Function::New(env, bsonToJson<ISA::SSE42>);
		BJTrans<ISA::SSE42>::Init(env, exports);
		isa = "SSE4.2";
	} else if (supports<ISA::SSE2>()) {
		fn = Napi::Function::New(env, bsonToJson<ISA::SSE2>);
		BJTrans<ISA::SSE2>::Init(env, exports);
		isa = "SSE2";
	} else {
		fn = Napi::Function::New(env, bsonToJson<ISA::BASELINE>);
		BJTrans<ISA::BASELINE>::Init(env, exports);
		isa = "Baseline";
	}

	exports.Set(Napi::String::New(env, "bsonToJson"), fn);
	exports.Set(Napi::String::New(env, "ISE"), isa);

	return exports;
}

NODE_API_MODULE(NODE_GYP_MODULE_NAME, Init)