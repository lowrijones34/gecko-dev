// |reftest| skip-if(!this.hasOwnProperty('Atomics')) -- Atomics is not enabled unconditionally
// Copyright 2015 Microsoft Corporation. All rights reserved.
// Copyright (C) 2017 Mozilla Corporation. All rights reserved.
// This code is governed by the license found in the LICENSE file.

/*---
esid: sec-atomics.xor
description: Testing descriptor property of Atomics.xor
includes: [propertyHelper.js]
features: [Atomics]
---*/

verifyWritable(Atomics, "xor");
verifyNotEnumerable(Atomics, "xor");
verifyConfigurable(Atomics, "xor");

reportCompare(0, 0);
