// |reftest| skip-if(!this.hasOwnProperty('Atomics')||!this.hasOwnProperty('SharedArrayBuffer')) -- Atomics,SharedArrayBuffer is not enabled unconditionally
// Copyright (C) 2017 Mozilla Corporation.  All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.

/*---
esid: sec-atomics.sub
description: Test Atomics.sub on arrays that allow atomic operations
includes: [testAtomics.js, testTypedArray.js]
features: [ArrayBuffer, arrow-function, Atomics, DataView, for-of, let, SharedArrayBuffer, TypedArray]
---*/

var sab = new SharedArrayBuffer(1024);
var ab = new ArrayBuffer(16);
var views = intArrayConstructors.slice();

testWithTypedArrayConstructors(function(TA) {
  // Make it interesting - use non-zero byteOffsets and non-zero indexes.

  var view = new TA(sab, 32, 20);
  var control = new TA(ab, 0, 2);

  view[8] = 100;
  assert.sameValue(Atomics.sub(view, 8, 10), 100,
    "Subtract positive number");
  assert.sameValue(view[8], 90);

  assert.sameValue(Atomics.sub(view, 8, -5), 90,
    "Subtract negative number, though result remains positive");
  assert.sameValue(view[8], 95);

  view[3] = -5;
  control[0] = -5;
  assert.sameValue(Atomics.sub(view, 3, 0), control[0],
    "Result is negative and subject to coercion");

  control[0] = 12345;
  view[3] = 12345;
  assert.sameValue(Atomics.sub(view, 3, 0), control[0],
    "Result is subject to chopping");

  control[0] = 123456789;
  view[3] = 123456789;
  assert.sameValue(Atomics.sub(view, 3, 0), control[0],
    "Result is subject to chopping");

  // In-bounds boundary cases for indexing
  testWithAtomicsInBoundsIndices(function(IdxGen) {
    let Idx = IdxGen(view);
    view.fill(0);
    // Atomics.store() computes an index from Idx in the same way as other
    // Atomics operations, not quite like view[Idx].
    Atomics.store(view, Idx, 37);
    assert.sameValue(Atomics.sub(view, Idx, 0), 37);
  });
}, views);

reportCompare(0, 0);
