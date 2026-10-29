// SPDX-License-Identifier: MIT
// Node render-harness stdout is a byte stream, bypassing Emscripten's text TTY.
Module["stdout"] = (byte) => require("fs").writeSync(1, new Uint8Array([byte]));
