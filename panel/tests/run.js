// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// run.js - runs every panel test with Node's built-in runner.
//
//     node panel/tests/run.js
//
// `node --test <folder>` is not accepted by every Node version and a glob is
// not expanded by every shell, so this lists the *.test.js files itself and
// hands them to `node --test` explicitly.  Exit code = the runner's.
'use strict';

const path = require('node:path');
const fs = require('node:fs');
const { spawnSync } = require('node:child_process');

const major = Number(process.versions.node.split('.')[0]);
if (!(major >= 18)) {
    console.error('The panel tests need Node 18 or later (node:test); this is ' + process.version + '.');
    process.exit(1);
}

const dir = __dirname;
const files = fs.readdirSync(dir)
    .filter((f) => f.endsWith('.test.js'))
    .sort()
    .map((f) => path.join(dir, f));

if (files.length === 0) {
    console.error('No *.test.js files in ' + dir);
    process.exit(1);
}

const result = spawnSync(process.execPath, ['--test'].concat(files), { stdio: 'inherit' });
if (result.error) {
    console.error(result.error.message);
    process.exit(1);
}
process.exit(result.status === null ? 1 : result.status);
