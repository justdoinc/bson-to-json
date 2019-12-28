//@ts-check
"use strict";

const assert = require("assert");
const bson = require("bson");

// Exercises all JSON types and nuances of JSON serialization.
const doc1 = {
	string: "string\tdata\n",
	objectId: new bson.ObjectId(),
	int: 1,
	int2: -2345718,
	number: Math.PI,
	date: new Date(1575271655028), // has a 0 in the millis (028)
	boolean: false,
	object: {
		embedded: "document"
	},
	array: [-2, "hey", new bson.ObjectId()],
	null: null,
	long: new bson.Long(0xFFFFFFF, 0xFFFFFFF),
	undefined: undefined, // omitted from result
	nan: NaN, // serializes to null
	infinity: Infinity, // serializes to null
	negInfinity: -Infinity, // serializes to null
	arrayOfUndef: [1, undefined] // undefined is preserved
};

// For using C++ debugger
global.describe = global.describe || function describe(label, fn) { fn(); };
global.it = global.it || function it(label, fn) { fn(); };

// TODO {a: NaN, b: Infinity, c: -Infinity} -> {a: null, b: null, c: null}
// TODO {arr: [1, undefined]} -> '{"arr": [1, null]}'

for (const [name, loc] of [["JS", "../index.js"]/*, ["C++", "../build/release/bsonToJson.node"]*/]) {
	const {bsonToJson} = require(loc);

	describe(`bson2json - ${name}`, function () {

		it("deserializes all JSON types", function () {
			const bsonBuffer = bson.serialize(doc1);
			const jsonBuffer = bsonToJson(bsonBuffer, false);

			const expected = JSON.parse(JSON.stringify(doc1));
			// The JSON string will contain 1152921500580315135, which parses to
			// 1152921500580315100.
			expected.long = doc1.long.toNumber();

			assert.deepEqual(
				JSON.parse(jsonBuffer.toString()),
				expected
			);
		});

		it("escapes strings properly", function () {
			const str = Buffer.allocUnsafe(0x7e);
			for (let i = 0; i < 0x7e; i++) str[i] = i;
			const obj = {str: str.toString()};

			const bsonBuffer = bson.serialize(obj);
			const jsonBuffer = bsonToJson(bsonBuffer, false);

			assert.deepEqual(jsonBuffer, Buffer.from(JSON.stringify(obj)));
			assert.equal(jsonBuffer.toString(), JSON.stringify(bson.deserialize(bsonBuffer)));
		});

		it("writes multi-byte characters properly", function () {
			const s1 = "𝌆"; // three bytes
			const s2 = "\uD834\udf06"; // same as s1
			const s3 = "\uDF06\uD834"; // encoded in BSON as ef bf bd (�)
			const s4 = "\uDEAD"; // encoded in BSON as ef bf bd (�)
			const s5 = "漢"; // two bytes
			const obj = {s1, s2, s3, s4, s5};

			const bsonBuffer = bson.serialize(obj);
			const jsonBuffer = bsonToJson(bsonBuffer, false);

			// Unlike the previous test, this can't use JSON.stringify for the
			// expectation because the lone surrogates are encoded into the BSON as
			// replacement characters.
			assert.deepEqual(jsonBuffer.toString(), JSON.stringify(bson.deserialize(bsonBuffer)));
		});
	});
}
