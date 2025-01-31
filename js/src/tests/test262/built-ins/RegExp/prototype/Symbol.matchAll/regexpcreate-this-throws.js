// |reftest| skip -- Symbol.matchAll is not supported
// Copyright (C) 2018 Peter Wong. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.
/*---
esid: pending
description: Re-throws errors while creating an internal RegExp
info: |
  RegExp.prototype [ @@matchAll ] ( string )
    [...]
    3. Return ? MatchAllIterator(R, string).

  MatchAllIterator ( R, O )
    [...]
    2. If ? IsRegExp(R) is true, then
      [...]
    3. Else,
      a. Let R be RegExpCreate(R, "g").
features: [Symbol.matchAll]
---*/

var obj = {
  toString() {
    throw new Test262Error();
  }
};

assert.throws(Test262Error, function() {
  RegExp.prototype[Symbol.matchAll].call(obj, '');
});


reportCompare(0, 0);
