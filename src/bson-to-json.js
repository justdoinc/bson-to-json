//@ts-check
'use strict';

const {transcode: transcodeBytes, Buffer} = require("buffer");
const Long = require("long");

const BSON_DATA_NUMBER = 1;
const BSON_DATA_STRING = 2;
const BSON_DATA_OBJECT = 3;
const BSON_DATA_ARRAY = 4;
const BSON_DATA_BINARY = 5;
const BSON_DATA_UNDEFINED = 6;
const BSON_DATA_OID = 7;
const BSON_DATA_BOOLEAN = 8;
const BSON_DATA_DATE = 9;
const BSON_DATA_NULL = 10;
const BSON_DATA_REGEXP = 11;
const BSON_DATA_DBPOINTER = 12;
const BSON_DATA_CODE = 13;
const BSON_DATA_SYMBOL = 14;
const BSON_DATA_CODE_W_SCOPE = 15;
const BSON_DATA_INT = 16;
const BSON_DATA_TIMESTAMP = 17;
const BSON_DATA_LONG = 18;
const BSON_DATA_DECIMAL128 = 19;
const BSON_DATA_MIN_KEY = 0xff;
const BSON_DATA_MAX_KEY = 0x7f;

const QUOTE = '"'.charCodeAt(0);
const COLON = ':'.charCodeAt(0);
const COMMA = ','.charCodeAt(0);
const OPENSQ = '['.charCodeAt(0);
const OPENCURL = '{'.charCodeAt(0);
const CLOSESQ = ']'.charCodeAt(0);
const CLOSECURL = '}'.charCodeAt(0);
const BACKSLASH = '\\'.charCodeAt(0);
const LOWERCASE_U = 'u'.charCodeAt(0);
const ZERO = '0'.charCodeAt(0);
const ONE = '1'.charCodeAt(0);

const TRUE = Buffer.from("true");
const FALSE = Buffer.from("false");
const NULL = Buffer.from("null");

// Returns the number of digits in a null-terminated string representation of v.
function nDigits(v) {
	if (v < 10) return 2;
	if (v < 100) return 3;
	if (v < 1_000) return 4;
	if (v < 10_000) return 5;
	if (v < 100_000) return 6;
	if (v < 1_000_000) return 7;
	if (v < 10_000_000) return 8;
	if (v < 100_000_000) return 9;
	if (v < 1_000_000_000) return 10;
	return 11;
}

// ECMA-262 Table 65 (sec. 24.5.2.2)
const ESCAPES = {
	0x08: 'b'.charCodeAt(0),
	0x09: 't'.charCodeAt(0),
	0x0a: 'n'.charCodeAt(0),
	0x0c: 'f'.charCodeAt(0),
	0x0d: 'r'.charCodeAt(0),
	0x22: '"'.charCodeAt(0),
	0x5c: '\\'.charCodeAt(0)
};

function readInt32LE(buffer, index) {
	return buffer[index] |
		(buffer[index + 1] << 8) |
		(buffer[index + 2] << 16) |
		(buffer[index + 3] << 24);
}

const tb = Buffer.allocUnsafeSlow(8);
const ta = new Float64Array(tb.buffer, tb.byteOffset, 1);
function readDoubleLE(buffer, index) {
	tb[0] = buffer[index];
	tb[1] = buffer[index + 1];
	tb[2] = buffer[index + 2];
	tb[3] = buffer[index + 3];
	tb[4] = buffer[index + 4];
	tb[5] = buffer[index + 5];
	tb[6] = buffer[index + 6];
	tb[7] = buffer[index + 7];
	return ta[0];
}

function hex(nibble) {
	return nibble + (nibble < 10 ? 48 : 87);
}

class Transcoder {
	constructor() {
		this.outIdx = 0;
	}

	/**
	 * @param {Buffer} buffer
	 */
	transcode(buffer, isArray = true) {
		const index = 0;

		const size = readInt32LE(buffer, index);

		if (size > buffer.length)
			throw new Error(`(bson size ${size} must be <= buffer length ${buffer.length})`);

		// Illegal end value
		if (buffer[index + size - 1] !== 0) {
			throw new Error("One object, sized correctly, with a spot for an EOO, but the EOO isn't 0x00");
		}

		const out = Buffer.alloc(1e8); // TODO overrun protection
		this.outIdx = 0;
		this.transcodeObject(out, buffer, index, isArray);
		return out.slice(0, this.outIdx);
	}

	/**
	 * Writes the bytes in `str` from `start` to `end` (exclusive) into `out`,
	 * escaping per ECMA-262 sec 24.5.2.2.
	 *
	 * Regarding [well-formed
	 * stringify](https://github.com/tc39/proposal-well-formed-stringify), the
	 * js-bson encoder uses Node.js' `buffer.write(value, index, "utf8")`, which
	 * converts unpaired surrogates to the byte sequence `ef bf bd`, which
	 * decodes to `0xfffd` (� REPLACEMENT CHARACTER, used to replace an unknown,
	 * unrecognized or unrepresentable character). Thus there's nothing we can
	 * do in the decoder to instead emit escape sequences.
	 *
	 * @param {Buffer} out
	 * @param {Buffer} str
	 * @param {number} start
	 * @param {number} end
	 * @private
	 */
	writeStringRange(out, str, start, end) {
		for (let i = start; i < end; i++) {
			const c = str[i];
			let xc;
			if (c >= 0x20 && c !== 0x22 && c !== 0x5c) { // no escape
				out[this.outIdx++] = c;
			} else if ((xc = ESCAPES[c])) { // single char escape
				out[this.outIdx++] = BACKSLASH;
				out[this.outIdx++] = xc;
			} else { // c < 0x20, control
				out[this.outIdx++] = BACKSLASH;
				out[this.outIdx++] = LOWERCASE_U;
				out[this.outIdx++] = ZERO;
				out[this.outIdx++] = ZERO;
				out[this.outIdx++] = (c & 0xF0) ? ONE : ZERO;
				out[this.outIdx++] = hex(c & 0xF);
			}
		}
	}

	/**
	 * This is the same speed as a 16B LUT and a 512B LUT, and doesn't pollute
	 * the cache. js-bson is still winning in the ObjectId benchmark though,
	 * despite having extra copying and a call into C++.
	 * @private
	 */
	writeObjectId(out, buffer, start) {
		for (let i = start; i < start + 12; i++) {
			const byte = buffer[i];
			const hi = byte >>> 4;
			const lo = byte & 0xF;
			out[this.outIdx++] = hex(hi);
			out[this.outIdx++] = hex(lo);
		}
	}

	addQuotedStringRange(out, buffer, valStart, valEnd) {
		out[this.outIdx++] = QUOTE;
		this.writeStringRange(out, buffer, valStart, valEnd);
		out[this.outIdx++] = QUOTE;
	}
	addQuotedVal(out, val) {
		out[this.outIdx++] = QUOTE;
		for (let i = 0; i < val.length; i++) out[this.outIdx++] = val[i];
		out[this.outIdx++] = QUOTE;
	}
	addVal(out, val) {
		for (let i = 0; i < val.length; i++) out[this.outIdx++] = val[i];
	}

	/**
	 * @param {Buffer} out
	 * @param {Buffer} buffer
	 * @param {number} index
	 * @param {boolean} isArray
	 * @private
	 */
	transcodeObject(out, buffer, index, isArray) {
		const bufLen = buffer.length;
		const size = readInt32LE(buffer, index);
		index += 4;

		if (size < 5 || size > bufLen)
			throw new Error('corrupt bson message');

		let arrIdx = 0;

		out[this.outIdx++] = isArray? OPENSQ : OPENCURL;

		while (true) {
			const elementType = buffer[index++];

			// If we get a zero it's the last byte, exit
			if (elementType === 0) break;

			if (arrIdx) {
				out[this.outIdx++] = COMMA;
			}

			if (isArray) {
				// Skip the number of digits in the key.
				index += nDigits(arrIdx);
			} else {
				// Name is a null-terminated string. TODO we can copy bytes as
				// we search.
				let nameStart = index;
				let nameEnd = index;
				while (buffer[nameEnd] !== 0x00 && nameEnd < bufLen) {
					nameEnd++;
				}
	
				if (nameEnd >= bufLen)
					throw new Error('Bad BSON Document: illegal CString');

				out[this.outIdx++] = QUOTE;
				this.writeStringRange(out, buffer, nameStart, nameEnd);
				out[this.outIdx++] = QUOTE;
				out[this.outIdx++] = COLON;

				index = nameEnd + 1;
			}

			switch (elementType) {
				case BSON_DATA_STRING: {
					const stringSize = readInt32LE(buffer, index);
					index += 4;
					if (
						stringSize <= 0 ||
						stringSize > bufLen - index ||
						buffer[index + stringSize - 1] !== 0 // TODO this is bad for cache access
					)
						throw new Error('bad string length in bson');

					// if (!validateUtf8(buffer, index, index + stringSize - 1))
					// 	throw new Error('Invalid UTF-8 string in BSON document');

					this.addQuotedStringRange(out, buffer, index, index + stringSize - 1);

					index += stringSize;
					break;
				}
				case BSON_DATA_OID: {
					out[this.outIdx++] = QUOTE;
					this.writeObjectId(out, buffer, index);
					out[this.outIdx++] = QUOTE;

					index += 12;
					break;
				}
				case BSON_DATA_INT: {
					const value = readInt32LE(buffer, index);
					// JS impl of fast_itoa is slower than this.
					this.addVal(out, Buffer.from(value.toString()));
					index += 4;
					break;
				}
				case BSON_DATA_NUMBER: {
					// const value = buffer.readDoubleLE(index); // not sure which is faster TODO
					const value = readDoubleLE(buffer, index);
					if (Number.isFinite(value)) {
						this.addVal(out, Buffer.from(value.toString()));
					} else {
						this.addVal(out, NULL);
					}
					index += 8;
					break;
				}
				case BSON_DATA_DATE: {
					const lowBits = readInt32LE(buffer, index);
					index += 4;
					const highBits = readInt32LE(buffer, index);
					index += 4;
					const ms = new Long(lowBits, highBits).toNumber();
					const value = Buffer.from(new Date(ms).toISOString());
					this.addQuotedVal(out, value);
					break;
				}
				case BSON_DATA_BOOLEAN: {
					if (buffer[index] !== 0 && buffer[index] !== 1)
						throw new Error('illegal boolean type value');
					const value = buffer[index++] === 1;
					this.addVal(out, value ? TRUE : FALSE);
					break;
				}
				case BSON_DATA_OBJECT: {
					const objectSize = readInt32LE(buffer, index);
					if (objectSize <= 0 || objectSize > bufLen - index)
						throw new Error('bad embedded document length in bson');

					this.transcodeObject(out, buffer, index, false);

					index += objectSize;
					break;
				}
				case BSON_DATA_ARRAY: {
					const objectSize = readInt32LE(buffer, index);

					this.transcodeObject(out, buffer, index, true);

					index += objectSize;

					if (buffer[index - 1] !== 0)
						throw new Error('invalid array terminator byte');
					break;
				}
				case BSON_DATA_NULL: {
					this.addVal(out, NULL);
					break;
				}
				case BSON_DATA_LONG: {
					const lowBits = readInt32LE(buffer, index);
					index += 4;
					const highBits = readInt32LE(buffer, index);
					index += 4;
					let vx;
					if (highBits === 0) {
						vx = lowBits;
					} else {
						vx = new Long(lowBits, highBits);
					}
					const value = Buffer.from(vx.toString());
					this.addVal(out, value);
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
					break;
				default:
					throw new Error('Detected unknown BSON type ' +
						elementType.toString(16));
			}

			arrIdx++;
		}

		out[this.outIdx++] = isArray ? CLOSESQ : CLOSECURL;
	}
}

exports.bsonToJson = function bsonToJson(doc, isArray) {
	const t = new Transcoder();
	return t.transcode(doc, isArray);
};